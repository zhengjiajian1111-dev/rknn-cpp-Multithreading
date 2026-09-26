#include "output/FfmpegPushSink.hpp"

#include <string.h>

extern "C"
{
#include <libavformat/avformat.h>
}

#include "common/Logger.hpp"
#include "common/WorkerThread.hpp"

static std::string avErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

int FfmpegPushSink::interruptCallback(void *opaque)
{
    FfmpegPushSink *self = static_cast<FfmpegPushSink *>(opaque);
    int64_t dl = self->deadline_ms_.load();
    return (dl > 0 && nowMs() > dl) ? 1 : 0;
}

bool FfmpegPushSink::open(const StreamInfo &info)
{
    close();
    const char *fmt = cfg_.type == "rtsp" ? "rtsp" : "flv";
    int ret = avformat_alloc_output_context2(&oc_, nullptr, fmt, cfg_.url.c_str());
    if (ret < 0 || !oc_)
    {
        LOGE("%s: alloc output context failed: %s", cfg_.name.c_str(), avErr(ret).c_str());
        oc_ = nullptr;
        return false;
    }
    oc_->interrupt_callback.callback = &FfmpegPushSink::interruptCallback;
    oc_->interrupt_callback.opaque = this;

    st_ = avformat_new_stream(oc_, nullptr);
    st_->time_base = AVRational{1, 1000};
    AVCodecParameters *par = st_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = info.width;
    par->height = info.height;
    par->format = AV_PIX_FMT_YUV420P;
    if (!info.header.empty())
    {
        par->extradata = (uint8_t *)av_mallocz(info.header.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        memcpy(par->extradata, info.header.data(), info.header.size());
        par->extradata_size = (int)info.header.size();
    }

    AVDictionary *opts = nullptr;
    if (cfg_.type == "rtsp")
        av_dict_set(&opts, "rtsp_transport", cfg_.rtsp_transport.c_str(), 0);
    else
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);

    deadline_ms_ = nowMs() + cfg_.timeout_ms;
    if (!(oc_->oformat->flags & AVFMT_NOFILE))
    {
        ret = avio_open2(&oc_->pb, cfg_.url.c_str(), AVIO_FLAG_WRITE, &oc_->interrupt_callback, nullptr);
        if (ret < 0)
        {
            LOGW("%s: connect %s failed: %s", cfg_.name.c_str(), cfg_.url.c_str(), avErr(ret).c_str());
            av_dict_free(&opts);
            close();
            return false;
        }
    }
    ret = avformat_write_header(oc_, &opts);
    av_dict_free(&opts);
    if (ret < 0)
    {
        std::string e = avErr(ret);
        // RTSP 服务器对"该路径已有推流者"通常返回 4xx(400/403/453...); RTMP 为 NetStream.Publish.BadName
        if (e.find("Server returned 4") != std::string::npos)
            LOGW("%s: server rejected publishing %s (%s) - the path may already be published by another session",
                 cfg_.name.c_str(), cfg_.url.c_str(), e.c_str());
        else
            LOGW("%s: write header to %s failed: %s", cfg_.name.c_str(), cfg_.url.c_str(), e.c_str());
        close();
        return false;
    }
    header_written_ = true;
    start_pts_ = -1;
    last_ts_ = -1;
    LOGI("%s: publishing to %s", cfg_.name.c_str(), cfg_.url.c_str());
    return true;
}

bool FfmpegPushSink::write(const EncodedPacket &pkt)
{
    if (!oc_)
        return false;
    if (start_pts_ < 0)
        start_pts_ = pkt.pts;
    int64_t ts = pkt.pts - start_pts_;
    if (ts <= last_ts_)
        ts = last_ts_ + 1; // 保证时间戳严格递增, 否则 muxer 会报错
    last_ts_ = ts;

    AVPacket *p = av_packet_alloc();
    if (av_new_packet(p, (int)pkt.data.size()) < 0)
    {
        av_packet_free(&p);
        return false;
    }
    memcpy(p->data, pkt.data.data(), pkt.data.size());
    p->stream_index = st_->index;
    p->pts = p->dts = av_rescale_q(ts, AVRational{1, 1000}, st_->time_base);
    if (pkt.key)
        p->flags |= AV_PKT_FLAG_KEY;

    deadline_ms_ = nowMs() + cfg_.timeout_ms;
    int ret = av_interleaved_write_frame(oc_, p);
    av_packet_free(&p);
    if (ret < 0)
    {
        LOGW("%s: write frame failed: %s", cfg_.name.c_str(), avErr(ret).c_str());
        return false;
    }
    return true;
}

void FfmpegPushSink::close()
{
    if (!oc_)
        return;
    deadline_ms_ = nowMs() + 2000; // 断线时 trailer/TEARDOWN 最多等 2 秒
    if (header_written_)
        av_write_trailer(oc_);
    if (!(oc_->oformat->flags & AVFMT_NOFILE) && oc_->pb)
        avio_closep(&oc_->pb);
    avformat_free_context(oc_);
    oc_ = nullptr;
    st_ = nullptr;
    header_written_ = false;
}
