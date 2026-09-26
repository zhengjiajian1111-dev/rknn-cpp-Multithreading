#ifndef OUTPUT_STREAMING_MGR_HPP
#define OUTPUT_STREAMING_MGR_HPP

#include <atomic>
#include <memory>
#include <vector>

#include "common/Config.hpp"
#include "common/Mbuffer.hpp"
#include "common/WorkerThread.hpp"
#include "output/StreamSink.hpp"
#include "output/VideoEncoder.hpp"

class PerfStat;

// 推流管理: 配置中每个启用的 [pushN] 对应一个 streamingWorker 线程
//   读源画面 images[i](拼接画面或单路结果) -> 叠加时间戳/统计 -> 编码(MPP/FFmpeg) -> 协议出口(RTMP/RTSP/GB28181)
//  * 视频处理结果以"最新帧 Mbuffer"的形式进入推流流程, 推流按自己的目标帧率取帧, 与推理速度解耦;
//  * 断线自动重连(指数退避), 重连后强制关键帧; 各推流项相互独立, 一路异常不影响其它路.
class StreamingMgr
{
public:
    StreamingMgr(const std::vector<PushConfig> &cfgs, bool use_rga);
    ~StreamingMgr();

    // mosaic: 拼接画面; channels[i]: 第 i 路推理结果
    int init(Mbuffer *mosaic, const std::vector<Mbuffer *> &channels);
    void start();
    void requestStop();
    void join();

private:
    struct Stream
    {
        PushConfig cfg;
        Mbuffer *src = nullptr;
        std::unique_ptr<IStreamSink> sink;
        std::unique_ptr<VideoEncoder> encoder;
        WorkerThread thread;
        PerfStat *st_encode = nullptr;
        PerfStat *st_send = nullptr;
        PerfStat *st_reconnect = nullptr;
    };

    void streamingWorker(Stream *s);
    static void overlayInfo(cv::Mat &img, const Stream &s, uint64_t frame_no);

    std::vector<PushConfig> cfgs_;
    bool use_rga_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::atomic<bool> running_{false};
};

#endif
