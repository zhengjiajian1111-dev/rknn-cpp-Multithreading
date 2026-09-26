#include "stream/StreamLoader.hpp"

#include <string.h>

#include <algorithm>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#if __has_include(<libavcodec/bsf.h>)
#include <libavcodec/bsf.h>
#endif
#ifdef HAVE_AVDEVICE
#include <libavdevice/avdevice.h>
#endif
}

#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"

static std::string avErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

static bool startsWith(const std::string &s, const char *prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

StreamLoader::StreamLoader(int channel, const SourceConfig &cfg, BlockingQueue<VideoPacketPtr> *out)
    : ch_(channel), cfg_(cfg), out_(out)
{
    std::string p = "ch" + std::to_string(ch_);
    st_read_ = PerfMonitor::instance().stat(p + ".demux");
    st_drop_ = PerfMonitor::instance().stat(p + ".drop.pkt");
    st_reconnect_ = PerfMonitor::instance().stat(p + ".reconnect");
}

StreamLoader::~StreamLoader()
{
    requestStop();
    join();
    closeInput();
}

void StreamLoader::start()
{
    running_ = true;
    thread_.start("ch" + std::to_string(ch_) + "-demux", [this] { run(); });
}

void StreamLoader::requestStop()
{
    // interruptCallback 检测到 running_=false 会让阻塞中的 av_read_frame 立即返回
    running_ = false;
}

void StreamLoader::join() { thread_.join(); }

int StreamLoader::interruptCallback(void *opaque)
{
    StreamLoader *self = static_cast<StreamLoader *>(opaque);
    if (!self->running_.load())
        return 1;
    int64_t dl = self->deadline_ms_.load();
    return (dl > 0 && nowMs() > dl) ? 1 : 0;
}

void StreamLoader::armDeadline(int ms) { deadline_ms_ = nowMs() + ms; }

bool StreamLoader::openInput()
{
    const std::string &url = cfg_.url;
    bool is_rtsp = startsWith(url, "rtsp://") || startsWith(url, "rtsps://");
    bool is_device = startsWith(url, "/dev/video");
    bool is_net = url.find("://") != std::string::npos && !startsWith(url, "file://");
    is_file_ = !is_net && !is_device;
    live_ = !is_file_;

    AVDictionary *opts = nullptr;
    decltype(av_find_input_format("")) ifmt = nullptr;
    if (is_device)
    {
#ifdef HAVE_AVDEVICE
        ifmt = av_find_input_format("v4l2");
#else
        LOGE("ch%d: %s needs libavdevice (rebuild with libavdevice-dev)", ch_, url.c_str());
        return false;
#endif
    }
    if (is_rtsp)
    {
        av_dict_set(&opts, "rtsp_transport", cfg_.rtsp_transport.c_str(), 0);
        // FFmpeg 5 起 socket 超时选项为 timeout, 4.x 为 stimeout(4.x 的 timeout 表示监听模式, 不能混用)
#if LIBAVFORMAT_VERSION_MAJOR >= 59
        av_dict_set(&opts, "timeout", std::to_string((int64_t)cfg_.timeout_ms * 1000).c_str(), 0);
#else
        av_dict_set(&opts, "stimeout", std::to_string((int64_t)cfg_.timeout_ms * 1000).c_str(), 0);
#endif
        av_dict_set(&opts, "buffer_size", "4194304", 0);
        av_dict_set(&opts, "max_delay", "500000", 0);
    }
    else if (is_net)
    {
        av_dict_set(&opts, "rw_timeout", std::to_string((int64_t)cfg_.timeout_ms * 1000).c_str(), 0);
    }
    if (live_)
    {
        // 实时流缩短探测时间, 加快首帧
        av_dict_set(&opts, "analyzeduration", "1000000", 0);
        av_dict_set(&opts, "probesize", "1048576", 0);
    }

    fmt_ = avformat_alloc_context();
    fmt_->interrupt_callback.callback = &StreamLoader::interruptCallback;
    fmt_->interrupt_callback.opaque = this;

    armDeadline(cfg_.timeout_ms);
    int ret = avformat_open_input(&fmt_, url.c_str(), ifmt, &opts);
    av_dict_free(&opts);
    if (ret < 0)
    {
        LOGW("ch%d: open '%s' failed: %s", ch_, url.c_str(), avErr(ret).c_str());
        fmt_ = nullptr; // 失败时 avformat_open_input 已释放上下文
        return false;
    }

    armDeadline(cfg_.timeout_ms * 2);
    ret = avformat_find_stream_info(fmt_, nullptr);
    if (ret < 0)
    {
        LOGW("ch%d: find stream info failed: %s", ch_, avErr(ret).c_str());
        return false;
    }

    video_idx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_idx_ < 0)
    {
        LOGW("ch%d: no video stream in '%s'", ch_, url.c_str());
        return false;
    }
    AVStream *st = fmt_->streams[video_idx_];

    auto params = std::make_shared<StreamParams>();
    const char *bsf_name = nullptr;
    if (st->codecpar->codec_id == AV_CODEC_ID_H264)
    {
        params->codec = CodecType::H264;
        bsf_name = "h264_mp4toannexb";
    }
    else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC)
    {
        params->codec = CodecType::H265;
        bsf_name = "hevc_mp4toannexb";
    }
    else
    {
        params->codec = CodecType::Other;
    }

    // MP4/FLV 中的 H.264/H.265 是 AVCC/HVCC 格式, MPP 需要 Annex-B(起始码)格式, 并且 SPS/PPS 要在关键帧前带内传输
    if (bsf_name)
    {
        const AVBitStreamFilter *filter = av_bsf_get_by_name(bsf_name);
        if (!filter || av_bsf_alloc(filter, &bsf_) < 0)
        {
            LOGE("ch%d: bitstream filter %s unavailable", ch_, bsf_name);
            return false;
        }
        avcodec_parameters_copy(bsf_->par_in, st->codecpar);
        bsf_->time_base_in = st->time_base;
        if ((ret = av_bsf_init(bsf_)) < 0)
        {
            LOGE("ch%d: init bsf failed: %s", ch_, avErr(ret).c_str());
            return false;
        }
    }

    params->par.reset(avcodec_parameters_alloc(), [](AVCodecParameters *p) { avcodec_parameters_free(&p); });
    avcodec_parameters_copy(params->par.get(), bsf_ ? bsf_->par_out : st->codecpar);
    params->width = st->codecpar->width;
    params->height = st->codecpar->height;
    AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
    double fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    if (fps <= 1 || fps > 240)
        fps = 25.0;
    params->fps = fps;
    params->generation = ++generation_;
    params_ = params;
    frame_dur_ms_ = std::max<int64_t>(1, (int64_t)(1000.0 / fps));

    first_pts_ = INT64_MIN;
    wait_keyframe_ = true;

    LOGI("ch%d: opened '%s' codec=%s %dx%d @%.2ffps (%s)", ch_, url.c_str(), avcodec_get_name(st->codecpar->codec_id),
         params->width, params->height, fps, live_ ? "live" : "file");
    return true;
}

void StreamLoader::closeInput()
{
    if (bsf_)
        av_bsf_free(&bsf_);
    if (fmt_)
        avformat_close_input(&fmt_);
    video_idx_ = -1;
}

void StreamLoader::pace(int64_t rel_ms)
{
    int64_t target = wall_start_ms_ + rel_ms;
    int64_t now = nowMs();
    // 下游阻塞导致落后超过 1 秒: 重新对齐时间基准, 避免之后快速追帧
    if (now - target > 1000)
    {
        wall_start_ms_ = now - rel_ms;
        return;
    }
    while (running_ && nowMs() < target)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<int64_t>(20, target - nowMs())));
}

bool StreamLoader::emitPacket(AVPacket *pkt)
{
    AVRational tb = bsf_ ? bsf_->time_base_out : fmt_->streams[video_idx_]->time_base;
    AVRational ms = {1, 1000};
    int64_t dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    int64_t raw = dts != AV_NOPTS_VALUE ? av_rescale_q(dts, tb, ms) : INT64_MIN;
    if (first_pts_ == INT64_MIN)
    {
        first_pts_ = raw != INT64_MIN ? raw : 0;
        wall_start_ms_ = nowMs();
    }
    int64_t rel = raw != INT64_MIN ? raw - first_pts_ : (last_pts_ - pts_offset_ + frame_dur_ms_);
    int64_t pts = rel + pts_offset_;
    if (is_file_ && cfg_.realtime)
        pace(rel);
    last_pts_ = std::max(last_pts_, pts);

    bool key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    st_read_->inc();
    // 解码器必须从关键帧开始喂, 否则会花屏
    if (wait_keyframe_ && !key)
    {
        st_drop_->inc();
        return true;
    }
    wait_keyframe_ = false;

    auto vp = std::make_shared<VideoPacket>();
    vp->data.assign(pkt->data, pkt->data + pkt->size);
    vp->pts = pts;
    vp->key = key;
    vp->params = params_;

    if (live_)
    {
        // 实时流: 队列满说明解码跟不上, 丢掉整个积压的 GOP 并等待下一个关键帧,
        // 既不会花屏, 也不会让延迟越积越大
        if (!out_->tryPush(vp))
        {
            size_t dropped = out_->clear();
            st_drop_->inc(dropped + 1);
            LOGW("ch%d: decoder too slow, dropped %zu packets, wait next keyframe", ch_, dropped + 1);
            wait_keyframe_ = true;
            if (key)
            {
                wait_keyframe_ = false;
                out_->tryPush(vp);
            }
        }
    }
    else
    {
        // 文件源: 阻塞写入(背压), 不丢包
        while (running_ && !out_->push(vp, 100))
        {
            if (out_->closed())
                return false;
        }
    }
    return true;
}

bool StreamLoader::pump()
{
    AVPacket *pkt = av_packet_alloc();
    AVPacket *out = av_packet_alloc();
    bool reconnect = true;
    while (running_)
    {
        armDeadline(cfg_.timeout_ms);
        int ret = av_read_frame(fmt_, pkt);
        if (ret == AVERROR_EOF && is_file_)
        {
            if (!cfg_.loop)
            {
                reconnect = false;
                break;
            }
            // 循环播放: 回到开头, pts 在上一轮末尾基础上继续累加
            int64_t start = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time : 0;
            if (av_seek_frame(fmt_, -1, start, AVSEEK_FLAG_BACKWARD) < 0)
            {
                LOGW("ch%d: seek to start failed, reopen", ch_);
                break;
            }
            if (bsf_)
                av_bsf_flush(bsf_);
            pts_offset_ = last_pts_ + frame_dur_ms_;
            first_pts_ = INT64_MIN;
            continue;
        }
        if (ret < 0)
        {
            if (running_)
                LOGW("ch%d: read frame failed: %s", ch_, avErr(ret).c_str());
            break;
        }
        if (pkt->stream_index != video_idx_)
        {
            av_packet_unref(pkt);
            continue;
        }
        bool ok = true;
        if (bsf_)
        {
            if (av_bsf_send_packet(bsf_, pkt) < 0)
            {
                av_packet_unref(pkt);
                continue;
            }
            while (ok && av_bsf_receive_packet(bsf_, out) == 0)
            {
                ok = emitPacket(out);
                av_packet_unref(out);
            }
        }
        else
        {
            ok = emitPacket(pkt);
            av_packet_unref(pkt);
        }
        if (!ok)
            break;
    }
    av_packet_free(&pkt);
    av_packet_free(&out);
    return reconnect;
}

void StreamLoader::run()
{
    int backoff = 500;
    bool first = true;
    while (running_)
    {
        state_ = first ? kConnecting : kReconnecting;
        first = false;
        if (!openInput())
        {
            closeInput();
            st_reconnect_->inc();
            LOGW("ch%d: retry in %d ms", ch_, backoff);
            interruptibleSleep(backoff, running_);
            backoff = std::min(backoff * 2, cfg_.reconnect_max_ms);
            continue;
        }
        backoff = 500;
        state_ = kStreaming;
        bool reconnect = pump();
        closeInput();
        if (!running_)
            break;
        if (!reconnect)
        {
            LOGI("ch%d: end of stream", ch_);
            auto eos = std::make_shared<VideoPacket>();
            eos->eos = true;
            eos->params = params_;
            out_->push(eos, 1000);
            state_ = kFinished;
            return;
        }
        st_reconnect_->inc();
        LOGW("ch%d: stream interrupted, reconnect in %d ms", ch_, backoff);
        interruptibleSleep(backoff, running_);
    }
}
