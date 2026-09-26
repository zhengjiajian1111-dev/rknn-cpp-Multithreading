#ifndef APP_APP_HPP
#define APP_APP_HPP

#include <atomic>
#include <memory>
#include <vector>

#include "common/BlockingQueue.hpp"
#include "common/Config.hpp"
#include "common/Mbuffer.hpp"
#include "decode/DecodeWorker.hpp"
#include "fusion/DetectionFusion.hpp"
#include "infer/InferWorker.hpp"
#include "infer/ModelManager.hpp"
#include "output/Compositor.hpp"
#include "output/StreamingMgr.hpp"
#include "stream/StreamLoader.hpp"

// 多路流处理总装:
//
//  [source i] StreamLoader --pktQueue--> DecodeWorker --decoded Mbuffer--> InferWorker --images[i]--+
//        (s1 拉流)            (有界队列)   (s2/s3 解码+转BGR)  (最新帧)   (s4 推理 + s5 融合)          |
//                                                                                                  v
//                                    Compositor(combineImage) --mosaic--> 主线程 imshow / StreamingMgr(RTMP/RTSP/GB28181)
//
// 初始化顺序(先消费者, 后生产者; 先重资源, 后外部连接):
//   1. 加载模型/创建 NPU 上下文: 最耗时、最容易失败(模型路径/驱动版本/内存), 失败直接退出, 不留下半初始化的网络连接;
//   2. 创建所有队列与 Mbuffer;
//   3. 启动推流 -> 拼接 -> 推理 -> 解码 线程(消费者先就绪);
//   4. 最后启动拉流线程(生产者), 首帧到达时下游已经全部就绪.
// 退出顺序相反: 先通知所有线程停止并关闭队列(唤醒阻塞者), 再按 生产者 -> 消费者 的顺序 join,
//   最后才销毁 NPU 上下文(确保没有线程还在使用它).
class App
{
public:
    App() = default;
    ~App() { stop(); }

    int init(const AppConfig &cfg);
    int run(const std::atomic<bool> &quit);
    void stop();

private:
    AppConfig cfg_;
    bool started_ = false;
    bool stopped_ = false;

    ModelManager models_;
    std::unique_ptr<DetectionFusion> fusion_;

    std::vector<std::unique_ptr<BlockingQueue<VideoPacketPtr>>> pkt_queues_;
    std::vector<std::unique_ptr<Mbuffer>> decoded_;
    std::vector<std::unique_ptr<Mbuffer>> images_;
    Mbuffer mosaic_;

    std::vector<std::unique_ptr<StreamLoader>> loaders_;
    std::vector<std::unique_ptr<DecodeWorker>> decoders_;
    std::vector<std::unique_ptr<InferWorker>> infers_;
    std::unique_ptr<Compositor> compositor_;
    std::unique_ptr<StreamingMgr> streaming_;
};

#endif
