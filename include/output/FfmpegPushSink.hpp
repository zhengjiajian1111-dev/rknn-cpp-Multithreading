#ifndef OUTPUT_FFMPEG_PUSH_SINK_HPP
#define OUTPUT_FFMPEG_PUSH_SINK_HPP

#include <atomic>

#include "output/StreamSink.hpp"

struct AVFormatContext;
struct AVStream;

// RTMP(FLV) / RTSP(ANNOUNCE+RECORD) 推流, 基于 libavformat
//  * 连接/写入都带超时(interrupt_callback), 网络异常时不会永久阻塞推流线程;
//  * 服务器拒绝(如路径已有人在推: 重复会话)时给出明确日志, 由上层退避重连;
//  * 重连前总是先 av_write_trailer + 关闭旧连接, 避免自己在服务器上残留重复会话.
class FfmpegPushSink : public IStreamSink
{
public:
    explicit FfmpegPushSink(const PushConfig &cfg) : cfg_(cfg) {}
    ~FfmpegPushSink() override { close(); }

    bool open(const StreamInfo &info) override;
    bool write(const EncodedPacket &pkt) override;
    void close() override;
    bool isOpen() const override { return oc_ != nullptr; }
    std::string describe() const override { return cfg_.type + " " + cfg_.url; }

private:
    static int interruptCallback(void *opaque);

    PushConfig cfg_;
    AVFormatContext *oc_ = nullptr;
    AVStream *st_ = nullptr;
    bool header_written_ = false;
    int64_t start_pts_ = -1;
    int64_t last_ts_ = -1;
    std::atomic<int64_t> deadline_ms_{0};
};

#endif
