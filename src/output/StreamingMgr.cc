#include "output/StreamingMgr.hpp"

#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>

#include "common/FpsController.hpp"
#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "output/FfmpegPushSink.hpp"
#include "output/Gb28181Sink.hpp"

std::unique_ptr<IStreamSink> createSink(const PushConfig &cfg)
{
    if (cfg.type == "gb28181")
        return std::unique_ptr<IStreamSink>(new Gb28181Sink(cfg));
    return std::unique_ptr<IStreamSink>(new FfmpegPushSink(cfg));
}

StreamingMgr::StreamingMgr(const std::vector<PushConfig> &cfgs, bool use_rga) : cfgs_(cfgs), use_rga_(use_rga) {}

StreamingMgr::~StreamingMgr()
{
    requestStop();
    join();
}

int StreamingMgr::init(Mbuffer *mosaic, const std::vector<Mbuffer *> &channels)
{
    for (const auto &cfg : cfgs_)
    {
        std::unique_ptr<Stream> s(new Stream());
        s->cfg = cfg;
        s->src = cfg.source == "mosaic" ? mosaic : channels[atoi(cfg.source.c_str())];
        s->sink = createSink(cfg);
        auto &pm = PerfMonitor::instance();
        s->st_encode = pm.stat(cfg.name + ".encode");
        s->st_send = pm.stat(cfg.name + ".send");
        s->st_reconnect = pm.stat(cfg.name + ".reconnect");
        LOGI("%s: %s, source=%s", cfg.name.c_str(), s->sink->describe().c_str(), cfg.source.c_str());
        streams_.push_back(std::move(s));
    }
    return 0;
}

void StreamingMgr::start()
{
    running_ = true;
    for (auto &s : streams_)
    {
        Stream *p = s.get();
        p->thread.start(p->cfg.name, [this, p] { streamingWorker(p); });
    }
}

void StreamingMgr::requestStop() { running_ = false; }

void StreamingMgr::join()
{
    for (auto &s : streams_)
        s->thread.join();
}

void StreamingMgr::overlayInfo(cv::Mat &img, const Stream &s, uint64_t frame_no)
{
    // 时间戳(右下角) + 推流统计(左下角); 顶部留给拼接画面的通道标签
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_now;
    localtime_r(&tv.tv_sec, &tm_now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    double scale = std::max(0.5, img.cols / 1920.0 * 0.8);
    int thick = scale > 0.7 ? 2 : 1;
    int baseline = 0;
    cv::Size tsz = cv::getTextSize(ts, cv::FONT_HERSHEY_SIMPLEX, scale, thick, &baseline);
    cv::Point org(img.cols - tsz.width - 12, img.rows - 12);
    cv::putText(img, ts, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(0, 0, 0), thick + 2);
    cv::putText(img, ts, org, cv::FONT_HERSHEY_SIMPLEX, scale, cv::Scalar(255, 255, 255), thick);

    char info[160];
    snprintf(info, sizeof(info), "%s %s | %.1f fps | enc %.1f ms | #%llu", s.cfg.name.c_str(), s.cfg.type.c_str(),
             s.st_send->rate(), s.st_encode->avgMs(), (unsigned long long)frame_no);
    cv::Point org2(12, img.rows - 12);
    cv::putText(img, info, org2, cv::FONT_HERSHEY_SIMPLEX, scale * 0.8, cv::Scalar(0, 0, 0), thick + 2);
    cv::putText(img, info, org2, cv::FONT_HERSHEY_SIMPLEX, scale * 0.8, cv::Scalar(0, 255, 255), thick);
}

void StreamingMgr::streamingWorker(Stream *s)
{
    const PushConfig &cfg = s->cfg;
    FpsController fps(cfg.fps);
    int backoff = 1000;
    int64_t next_retry = 0;
    int64_t start_ms = nowMs();
    uint64_t frame_no = 0;
    std::vector<EncodedPacket> pkts;

    while (running_)
    {
        fps.wait(&running_);
        if (!running_)
            break;
        int64_t now = nowMs();

        // GB28181 等"先建信令再等点播"的协议: 连接不依赖编码器
        if (!s->sink->isOpen() && !s->sink->needsHeaderToOpen() && now >= next_retry)
        {
            if (!s->sink->open(StreamInfo()))
            {
                s->st_reconnect->inc();
                next_retry = now + backoff;
                backoff = std::min(backoff * 2, cfg.reconnect_max_ms);
                continue;
            }
            backoff = 1000;
        }
        // 没有人点播 / 处于重连退避期时不编码, 节省 CPU/VPU
        if (!s->sink->wantsMedia())
            continue;
        if (!s->sink->isOpen() && now < next_retry)
            continue;

        FrameData fd;
        if (!s->src->peek(fd))
            continue;

        // 源画面被多个消费者共享, 叠加 OSD 前必须拷贝(resize 本身会生成新图像)
        int w = (cfg.width > 0 ? cfg.width : fd.img.cols) & ~1;
        int h = (cfg.height > 0 ? cfg.height : fd.img.rows) & ~1;
        cv::Mat img;
        if (fd.img.cols != w || fd.img.rows != h)
            cv::resize(fd.img, img, cv::Size(w, h));
        else
            img = fd.img.clone();
        if (cfg.overlay)
            overlayInfo(img, *s, frame_no);

        // 分辨率变化(或首次): 重建编码器, 推流连接也要按新参数重建
        if (!s->encoder || s->encoder->width() != w || s->encoder->height() != h)
        {
            s->encoder = createEncoder(cfg.encoder, use_rga_, w, h, (int)cfg.fps, cfg.bitrate_kbps, cfg.gop);
            if (!s->encoder)
            {
                LOGE("%s: create encoder failed, retry later", cfg.name.c_str());
                interruptibleSleep(1000, running_);
                continue;
            }
            if (s->sink->isOpen() && s->sink->needsHeaderToOpen())
                s->sink->close();
        }
        if (s->sink->takeKeyframeRequest())
            s->encoder->requestKeyframe();

        pkts.clear();
        {
            ScopedTimer t(s->st_encode);
            if (!s->encoder->encode(img, now - start_ms, pkts))
                LOGW("%s: encode failed", cfg.name.c_str());
        }
        frame_no++;

        for (auto &pkt : pkts)
        {
            if (!s->sink->isOpen())
            {
                // 等到关键帧再建立连接: 保证 extradata 已知, 且对端收到的第一帧就能解码
                if (!pkt.key)
                {
                    s->encoder->requestKeyframe();
                    continue;
                }
                if (nowMs() < next_retry)
                    continue;
                StreamInfo info;
                info.width = w;
                info.height = h;
                info.fps = cfg.fps;
                info.header = s->encoder->header();
                if (!s->sink->open(info))
                {
                    s->st_reconnect->inc();
                    LOGW("%s: connect failed, retry in %d ms", cfg.name.c_str(), backoff);
                    next_retry = nowMs() + backoff;
                    backoff = std::min(backoff * 2, cfg.reconnect_max_ms);
                    continue;
                }
                backoff = 1000;
            }
            bool ok;
            {
                ScopedTimer t(s->st_send);
                ok = s->sink->write(pkt);
            }
            if (!ok)
            {
                // 连接断开: 关闭旧连接(避免服务器上残留重复会话), 退避后重连, 重连首帧为关键帧
                LOGW("%s: connection lost, reconnect in %d ms", cfg.name.c_str(), backoff);
                s->sink->close();
                s->st_reconnect->inc();
                next_retry = nowMs() + backoff;
                backoff = std::min(backoff * 2, cfg.reconnect_max_ms);
                s->encoder->requestKeyframe();
                break;
            }
        }
    }
    s->sink->close();
    s->encoder.reset();
    LOGI("%s: stopped", cfg.name.c_str());
}
