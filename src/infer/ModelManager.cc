#include "infer/ModelManager.hpp"

#include <string.h>

#include <exception>
#include <future>

#include "common/Logger.hpp"
#include "common/PerfMonitor.hpp"

static const int kNpuCores = 3; // RK3588/RK3588S

static rknn_core_mask coreMaskFromString(const std::string &s, int &round_robin)
{
    if (s == "auto")
        return (rknn_core_mask)(1 << (round_robin++ % kNpuCores)); // 实例间轮询绑定 core0/1/2
    if (s == "any")
        return RKNN_NPU_CORE_AUTO; // 交给驱动调度
    if (s == "0")
        return RKNN_NPU_CORE_0;
    if (s == "1")
        return RKNN_NPU_CORE_1;
    if (s == "2")
        return RKNN_NPU_CORE_2;
    if (s == "0_1")
        return RKNN_NPU_CORE_0_1;
    if (s == "0_1_2")
        return RKNN_NPU_CORE_0_1_2;
    LOGW("unknown core '%s', use auto", s.c_str());
    return RKNN_NPU_CORE_AUTO;
}

ModelManager::Lease::Lease(ModelManager::Slot &s) : slot_(s)
{
    ScopedTimer t(s.st_wait);
    std::unique_lock<std::mutex> lock(s.mtx);
    s.cv.wait(lock, [&] { return !s.idle.empty(); });
    model_ = s.idle.back();
    s.idle.pop_back();
}

ModelManager::Lease::~Lease()
{
    {
        std::lock_guard<std::mutex> lock(slot_.mtx);
        slot_.idle.push_back(model_);
    }
    slot_.cv.notify_one();
}

int ModelManager::init(const std::vector<ModelConfig> &models, bool pre_rga, int pool_threads)
{
    pre_rga_ = pre_rga;
    int round_robin = 0;
    int total_instances = 0;
    for (const auto &cfg : models)
    {
        std::unique_ptr<Slot> slot(new Slot());
        slot->cfg = cfg;
        slot->st_wait = PerfMonitor::instance().stat(cfg.name + ".wait");
        auto labels = std::make_shared<const std::vector<std::string>>(loadLabels(cfg.labels));
        if (labels->empty())
            LOGW("[%s] no labels loaded from %s", cfg.name.c_str(), cfg.labels.c_str());

        for (int i = 0; i < cfg.instances; i++)
        {
            std::unique_ptr<RknnLite> m(new RknnLite(cfg, i, labels));
            RknnLite *master = i == 0 ? nullptr : slot->instances[0].get();
            if (m->init(master, coreMaskFromString(cfg.core, round_robin)) != 0)
            {
                LOGE("[%s] instance %d init failed", cfg.name.c_str(), i);
                return -1;
            }
            slot->idle.push_back(m.get());
            slot->instances.push_back(std::move(m));
        }
        total_instances += cfg.instances;
        slots_.push_back(std::move(slot));
    }

    // 线程数 = 实例总数: 每个线程最多持有一个实例, 持有者不会等待其它资源, 因此不会死锁
    int threads = pool_threads > 0 ? pool_threads : total_instances;
    pool_.reset(new dpool::ThreadPool(threads));
    LOGI("model manager ready: %zu model(s), %d instance(s), pool threads=%d", slots_.size(), total_instances, threads);
    return 0;
}

int ModelManager::runOne(Slot &slot, const cv::Mat &img, InferBuffers &buf, detect_result_group_t &group)
{
    memset(&group, 0, sizeof(group)); // 失败时结果为空, 不残留上一帧的框
    try
    {
        // 前后处理只用工作区和各实例相同的模型参数, 借用 master 执行即可, 不占实例
        const RknnLite &model = *slot.instances[0];
        if (!model.prepare(img, buf))
            return -1;
        {
            // 只有 NPU 这一步借实例: 用完立即归还, 其它线程马上可以用这个核心
            Lease lease(slot);
            if (lease->run(buf) != 0)
                return -1;
        }
        model.decode(buf, group);
        return 0;
    }
    catch (const std::exception &e)
    {
        LOGE("[%s] inference exception: %s", slot.cfg.name.c_str(), e.what());
        return -1;
    }
}

int ModelManager::inferAll(const cv::Mat &img, std::vector<detect_result_group_t> &groups, Workspace &ws)
{
    groups.resize(slots_.size());
    if (slots_.empty())
        return -1;
    if (ws.size() != slots_.size())
    {
        ws.assign(slots_.size(), InferBuffers());
        for (auto &b : ws)
            b.use_rga = pre_rga_;
    }
    // 单模型: 直接在调用线程执行, 省去一次线程切换(多路并发靠多个推理线程)
    if (slots_.size() == 1)
        return runOne(*slots_[0], img, ws[0], groups[0]);

    // 多模型: 并发提交到线程池, 各模型分别占用自己绑定的 NPU 核心
    std::vector<std::future<int>> futures;
    futures.reserve(slots_.size());
    for (size_t k = 0; k < slots_.size(); k++)
    {
        Slot *slot = slots_[k].get();
        InferBuffers *buf = &ws[k];
        detect_result_group_t *g = &groups[k];
        futures.push_back(pool_->submit([this, slot, buf, g, &img]() { return runOne(*slot, img, *buf, *g); }));
    }
    // 等待全部完成: img/ws/groups 被任务引用, 必须全部结束才能返回
    int ret = 0;
    for (auto &f : futures)
    {
        if (f.get() != 0)
            ret = -1;
    }
    return ret;
}

void ModelManager::shutdown()
{
    // 先停线程池(确保没有任务在使用上下文), 再按 dup 实例 -> master 的顺序销毁上下文
    pool_.reset();
    for (auto &slot : slots_)
    {
        while (!slot->instances.empty())
            slot->instances.pop_back();
        slot->idle.clear();
    }
    slots_.clear();
}
