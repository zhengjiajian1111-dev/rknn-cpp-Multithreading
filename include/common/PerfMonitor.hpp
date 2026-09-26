#ifndef COMMON_PERF_MONITOR_HPP
#define COMMON_PERF_MONITOR_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// 单个统计项: 计数 + 耗时(无锁累加, 热路径开销极小)
class PerfStat
{
public:
    void add(double ms)
    {
        uint64_t us = ms > 0 ? (uint64_t)(ms * 1000.0) : 0;
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_us_.fetch_add(us, std::memory_order_relaxed);
        uint64_t prev = max_us_.load(std::memory_order_relaxed);
        while (us > prev && !max_us_.compare_exchange_weak(prev, us, std::memory_order_relaxed))
        {
        }
    }
    void inc(uint64_t n = 1) { count_.fetch_add(n, std::memory_order_relaxed); }

    // 最近一个统计窗口(1 秒)的结果, 供 OSD 叠加显示
    double rate() const { return rate_.load(); }
    double avgMs() const { return avg_ms_.load(); }
    uint64_t total() const { return total_.load(); }

private:
    friend class PerfMonitor;
    std::atomic<uint64_t> count_{0}, sum_us_{0}, max_us_{0};
    std::atomic<double> rate_{0}, avg_ms_{0};
    std::atomic<uint64_t> total_{0};
    // 打印窗口累计(仅 reporter 线程访问)
    uint64_t win_count_ = 0, win_sum_us_ = 0, win_max_us_ = 0;
};

// 性能监控与瓶颈定位:
//  * 各阶段用 "ch0.decode"、"model0.npu"、"push0.encode" 之类的名字登记耗时/计数;
//  * 周期打印每个阶段的 调用频率 / 平均耗时 / 最大耗时 / 负载(=频率*平均耗时, 即该阶段占用
//    一个线程的比例), 负载接近 100% 的阶段即为瓶颈; 同时打印各级缓冲的丢帧计数,
//    "在哪一级开始丢帧" 就说明它的下游处理不过来.
class PerfMonitor
{
public:
    static PerfMonitor &instance();

    // 返回的指针在进程生命周期内有效, 可在热路径缓存
    PerfStat *stat(const std::string &name);

    void start(int print_interval_s);
    void stop();

private:
    PerfMonitor() = default;
    ~PerfMonitor();
    void run();
    void report(double seconds);

    std::mutex mtx_;
    std::map<std::string, std::unique_ptr<PerfStat>> stats_;
    std::thread th_;
    std::mutex run_mtx_;
    std::condition_variable run_cv_;
    bool running_ = false;
    int print_interval_s_ = 5;
};

// RAII 计时: 作用域结束时把耗时记入 stat
class ScopedTimer
{
public:
    explicit ScopedTimer(PerfStat *s) : s_(s), t0_(std::chrono::steady_clock::now()) {}
    ~ScopedTimer()
    {
        if (s_)
            s_->add(elapsedMs());
    }
    double elapsedMs() const
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
    }

private:
    PerfStat *s_;
    std::chrono::steady_clock::time_point t0_;
};

#endif
