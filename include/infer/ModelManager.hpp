#ifndef INFER_MODEL_MANAGER_HPP
#define INFER_MODEL_MANAGER_HPP

#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

#include "common/Config.hpp"
#include "common/ThreadPool.hpp"
#include "infer/rknn_lite.hpp"

class PerfStat;

// 多模型并行推理管理
//  * 每个模型持有 N 个 RknnLite 实例(实例池), 实例按配置绑定 NPU 核心:
//      core=auto 时所有模型的实例在 core0/1/2 上轮询分配, 保证三个核心负载均衡;
//  * inferAll(): 把同一帧"并发提交"给所有模型(线程池), 然后"等待"全部完成, 得到 g1..gN;
//  * 多路推理线程并发调用 inferAll 时, 实例池保证每个上下文同一时刻只被一个线程使用,
//    实例数即该模型 NPU 阶段的最大并发度;
//  * 每次推理: 前处理(不占实例) -> 借实例, NPU 推理, 还实例 -> 后处理(不占实例),
//    实例只在 NPU 真正工作时被占用, 前后处理的 CPU 时间不会让 NPU 核心空等.
class ModelManager
{
public:
    // 推理工作区: 每个模型一个 InferBuffers, 由每路推理线程各自持有(同一时刻只被一次 inferAll 使用)
    using Workspace = std::vector<InferBuffers>;

    ModelManager() = default;
    ~ModelManager() { shutdown(); }

    // pre_rga: 前处理是否用 RGA(失败时按工作区自动回退 OpenCV)
    int init(const std::vector<ModelConfig> &models, bool pre_rga, int pool_threads);
    int inferAll(const cv::Mat &img, std::vector<detect_result_group_t> &groups, Workspace &ws);
    void shutdown();

    size_t size() const { return slots_.size(); }
    const ModelConfig &config(size_t i) const { return slots_[i]->cfg; }

private:
    struct Slot
    {
        ModelConfig cfg;
        std::vector<std::unique_ptr<RknnLite>> instances; // [0] 为 master(rknn_init), 其余为 dup
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<RknnLite *> idle;
        PerfStat *st_wait = nullptr;
    };

    // RAII 租用一个空闲实例, 析构时归还
    class Lease
    {
    public:
        Lease(ModelManager::Slot &s);
        ~Lease();
        RknnLite *operator->() const { return model_; }

    private:
        Slot &slot_;
        RknnLite *model_;
    };

    int runOne(Slot &slot, const cv::Mat &img, InferBuffers &buf, detect_result_group_t &group);

    std::vector<std::unique_ptr<Slot>> slots_;
    bool pre_rga_ = true;
    std::unique_ptr<dpool::ThreadPool> pool_;
};

#endif
