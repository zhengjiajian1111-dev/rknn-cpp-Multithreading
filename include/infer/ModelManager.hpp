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
//    实例数即该模型的最大并发度.
class ModelManager
{
public:
    ModelManager() = default;
    ~ModelManager() { shutdown(); }

    int init(const std::vector<ModelConfig> &models, bool use_rga, int pool_threads);
    int inferAll(const cv::Mat &img, std::vector<detect_result_group_t> &groups);
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

    int runOne(Slot &slot, const cv::Mat &img, detect_result_group_t &group);

    std::vector<std::unique_ptr<Slot>> slots_;
    std::unique_ptr<dpool::ThreadPool> pool_;
};

#endif
