#ifndef INFER_INFER_WORKER_HPP
#define INFER_INFER_WORKER_HPP

#include <atomic>
#include <vector>

#include "common/Config.hpp"
#include "common/Mbuffer.hpp"
#include "common/WorkerThread.hpp"
#include "fusion/DetectionFusion.hpp"
#include "infer/ModelManager.hpp"

class PerfStat;

// s4. rknn_infer 线程(每路一个)
//   从 Mbuffer 读最新 BGR 帧 -> 目标帧率控制 -> (跳帧)多模型并发推理 -> 融合 -> 画框 -> 写输出 Mbuffer(images[i])
//  * 跳帧推理: 每 infer_interval 帧推理一次, 中间帧复用最近一次的融合结果(结果复用), 显示/推流帧率不受 NPU 限制;
//  * 结果时效: 复用结果超过 reuse_max_ms 不再绘制, 避免目标离开后残留旧框.
class InferWorker
{
public:
    InferWorker(int channel, const InferConfig &cfg, Mbuffer *in, Mbuffer *out, ModelManager *models,
                const DetectionFusion *fusion);
    ~InferWorker();

    void start();
    void requestStop();
    void join();

private:
    void run();

    int ch_;
    InferConfig cfg_;
    Mbuffer *in_;
    Mbuffer *out_;
    ModelManager *models_;
    const DetectionFusion *fusion_;
    WorkerThread thread_;
    std::atomic<bool> running_{false};

    PerfStat *st_infer_ = nullptr;
    PerfStat *st_fusion_ = nullptr;
    PerfStat *st_out_ = nullptr;
    PerfStat *st_drop_frame_ = nullptr;
    PerfStat *st_drop_fps_ = nullptr;
    PerfStat *st_reuse_ = nullptr;
};

#endif
