#include "common/PerfMonitor.hpp"

#include <stdio.h>

#include <fstream>
#include <sstream>
#include <vector>

#include "common/Logger.hpp"

PerfMonitor &PerfMonitor::instance()
{
    static PerfMonitor inst;
    return inst;
}

PerfMonitor::~PerfMonitor() { stop(); }

PerfStat *PerfMonitor::stat(const std::string &name)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto &p = stats_[name];
    if (!p)
        p.reset(new PerfStat());
    return p.get();
}

void PerfMonitor::start(int print_interval_s)
{
    std::lock_guard<std::mutex> lock(run_mtx_);
    if (running_)
        return;
    print_interval_s_ = print_interval_s;
    running_ = true;
    th_ = std::thread([this] {
        setThreadName("perf");
        run();
    });
}

void PerfMonitor::stop()
{
    {
        std::lock_guard<std::mutex> lock(run_mtx_);
        if (!running_)
            return;
        running_ = false;
    }
    run_cv_.notify_all();
    if (th_.joinable())
        th_.join();
}

void PerfMonitor::run()
{
    auto last = std::chrono::steady_clock::now();
    auto last_print = last;
    while (true)
    {
        {
            std::unique_lock<std::mutex> lock(run_mtx_);
            if (run_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return !running_; }))
                break;
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        // 1 秒窗口: 更新 OSD 用的实时值, 同时累积到打印窗口
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto &kv : stats_)
            {
                PerfStat *s = kv.second.get();
                uint64_t c = s->count_.exchange(0);
                uint64_t us = s->sum_us_.exchange(0);
                uint64_t mx = s->max_us_.exchange(0);
                s->rate_ = dt > 0 ? c / dt : 0;
                s->avg_ms_ = c ? us / 1000.0 / c : 0;
                s->total_ += c;
                s->win_count_ += c;
                s->win_sum_us_ += us;
                if (mx > s->win_max_us_)
                    s->win_max_us_ = mx;
            }
        }

        double since_print = std::chrono::duration<double>(now - last_print).count();
        if (print_interval_s_ > 0 && since_print >= print_interval_s_)
        {
            report(since_print);
            last_print = now;
        }
    }
}

static std::string readNpuLoad()
{
    // 需要 root 且挂载 debugfs; 读不到就忽略
    std::ifstream f("/sys/kernel/debug/rknpu/load");
    if (!f)
        return "";
    std::string line;
    std::getline(f, line);
    return line;
}

void PerfMonitor::report(double seconds)
{
    std::ostringstream oss;
    char buf[256];
    std::vector<std::string> hot;

    snprintf(buf, sizeof(buf), "\n==================== perf (last %.1fs) ====================\n", seconds);
    oss << buf;
    snprintf(buf, sizeof(buf), "%-22s %9s %9s %9s %7s %10s\n", "stage", "rate/s", "avg(ms)", "max(ms)", "load", "total");
    oss << buf;

    std::lock_guard<std::mutex> lock(mtx_);
    for (auto &kv : stats_)
    {
        PerfStat *s = kv.second.get();
        double rate = s->win_count_ / seconds;
        if (s->win_sum_us_ > 0)
        {
            double avg = s->win_sum_us_ / 1000.0 / (s->win_count_ ? s->win_count_ : 1);
            double load = rate * avg / 1000.0; // 该阶段占用单线程时间的比例
            snprintf(buf, sizeof(buf), "%-22s %9.1f %9.2f %9.2f %6.0f%% %10llu\n", kv.first.c_str(), rate, avg,
                     s->win_max_us_ / 1000.0, load * 100.0, (unsigned long long)s->total_.load());
            if (load > 0.85)
                hot.push_back(kv.first);
        }
        else
        {
            // 纯计数项(丢帧、复用等)
            snprintf(buf, sizeof(buf), "%-22s %9.1f %9s %9s %7s %10llu\n", kv.first.c_str(), rate, "-", "-", "-",
                     (unsigned long long)s->total_.load());
        }
        oss << buf;
        s->win_count_ = 0;
        s->win_sum_us_ = 0;
        s->win_max_us_ = 0;
    }

    std::string npu = readNpuLoad();
    if (!npu.empty())
        oss << npu << "\n";
    if (!hot.empty())
    {
        oss << "bottleneck(load>85%):";
        for (auto &h : hot)
            oss << " " << h;
        oss << "\n";
    }
    oss << "hint: *.drop.pkt 增长=解码跟不上, *.drop.frame 增长=推理跟不上, *.reuse=跳帧复用结果";
    LOGI("%s", oss.str().c_str());
}
