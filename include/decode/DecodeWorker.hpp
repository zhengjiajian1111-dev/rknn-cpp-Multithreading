#ifndef DECODE_DECODE_WORKER_HPP
#define DECODE_DECODE_WORKER_HPP

#include <atomic>
#include <memory>
#include <vector>

#include "common/BlockingQueue.hpp"
#include "common/Config.hpp"
#include "common/Mbuffer.hpp"
#include "common/WorkerThread.hpp"
#include "decode/VideoDecoder.hpp"

class PerfStat;

// 解码线程(每路一个): 从包队列取包 -> 解码 -> mpp_decoder_cb 转 BGR -> 写入 Mbuffer
// 拉流与解码拆成两个线程: 网络抖动不会阻塞解码, 解码变慢也不会让 socket 缓冲区溢出.
class DecodeWorker
{
public:
    DecodeWorker(int channel, const DecoderConfig &cfg, bool use_rga, BlockingQueue<VideoPacketPtr> *in,
                 Mbuffer *out);
    ~DecodeWorker();

    void start();
    void requestStop();
    void join();

    uint64_t decodedFrames() const { return frames_.load(); }

private:
    void run();
    // s3. 解码回调: 拷贝 YUV -> cvtColor(NV12->BGR) -> 写 Mbuffer.img
    static void mpp_decoder_cb(void *userdata, const DecodedFrame &frame);
    void onFrame(const DecodedFrame &frame);

    int ch_;
    DecoderConfig cfg_;
    bool use_rga_;
    BlockingQueue<VideoPacketPtr> *in_;
    Mbuffer *out_;
    WorkerThread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};

    std::unique_ptr<VideoDecoder> decoder_;
    std::shared_ptr<const StreamParams> cur_params_;
    std::vector<uint8_t> yuv_; // 去 stride 后的连续 NV12 数据(CPU 路径复用)

    PerfStat *st_decode_ = nullptr;
    PerfStat *st_cvt_ = nullptr;
};

#endif
