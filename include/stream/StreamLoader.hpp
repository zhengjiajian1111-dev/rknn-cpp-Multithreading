#ifndef STREAM_STREAM_LOADER_HPP
#define STREAM_STREAM_LOADER_HPP

#include <atomic>
#include <string>

#include "common/BlockingQueue.hpp"
#include "common/Config.hpp"
#include "common/WorkerThread.hpp"
#include "stream/StreamTypes.hpp"

struct AVFormatContext;
struct AVBSFContext;
struct AVPacket;
class PerfStat;

// s1. StreamLoader: 基于 FFmpeg 的拉流/读包线程(avformat_open_input + av_read_frame)
//  * 支持 RTSP/RTMP/HTTP-FLV/MP4 文件/v4l2 设备;
//  * H.264/H.265 统一经 *_mp4toannexb 转为 Annex-B, 直接送给 MPP;
//  * 断流/超时/读错误 -> 指数退避重连; 文件源可循环播放, 并按 pts 节奏读取;
//  * 实时流在解码跟不上时按 GOP 丢包(清空队列并等待下一个关键帧), 避免花屏和延迟累积.
class StreamLoader
{
public:
    enum State
    {
        kConnecting = 0,
        kStreaming = 1,
        kReconnecting = 2,
        kFinished = 3,
    };

    StreamLoader(int channel, const SourceConfig &cfg, BlockingQueue<VideoPacketPtr> *out);
    ~StreamLoader();

    void start();
    void requestStop();
    void join();

    State state() const { return (State)state_.load(); }
    bool finished() const { return state_.load() == kFinished; }
    const SourceConfig &config() const { return cfg_; }

private:
    void run();
    bool openInput();
    void closeInput();
    // 返回 false 表示需要重连
    bool pump();
    bool emitPacket(AVPacket *pkt);
    void pace(int64_t pts_ms);

    static int interruptCallback(void *opaque);
    void armDeadline(int ms);

    int ch_;
    SourceConfig cfg_;
    BlockingQueue<VideoPacketPtr> *out_;
    WorkerThread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> state_{kConnecting};
    std::atomic<int64_t> deadline_ms_{0};

    AVFormatContext *fmt_ = nullptr;
    AVBSFContext *bsf_ = nullptr;
    int video_idx_ = -1;
    bool is_file_ = false;
    bool live_ = true;
    std::shared_ptr<const StreamParams> params_;
    uint64_t generation_ = 0;

    // pts 处理: 循环播放时累加偏移, 保证下游 pts 单调递增
    int64_t pts_offset_ = 0;
    int64_t last_pts_ = 0;
    int64_t first_pts_ = INT64_MIN;
    int64_t wall_start_ms_ = 0;
    int64_t frame_dur_ms_ = 40;
    bool wait_keyframe_ = true;

    PerfStat *st_read_ = nullptr;
    PerfStat *st_drop_ = nullptr;
    PerfStat *st_reconnect_ = nullptr;
};

#endif
