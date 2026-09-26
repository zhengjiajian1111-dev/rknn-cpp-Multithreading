#ifndef COMMON_WORKER_THREAD_HPP
#define COMMON_WORKER_THREAD_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <thread>

#include "common/Logger.hpp"

// 线程生命周期管理(避免 std::terminate):
//  1. std::thread 在析构时若仍 joinable 会直接调用 std::terminate -> 析构函数中保证 join;
//  2. 线程函数中逃逸的异常同样会导致 std::terminate -> 线程入口统一 catch 并记录日志;
//  3. 不使用 detach, 所有线程都在退出流程中被显式 join, 资源释放顺序可控.
class WorkerThread
{
public:
    WorkerThread() = default;
    ~WorkerThread() { join(); }

    WorkerThread(const WorkerThread &) = delete;
    WorkerThread &operator=(const WorkerThread &) = delete;

    void start(const std::string &name, std::function<void()> fn)
    {
        join();
        name_ = name;
        th_ = std::thread([name, fn]() {
            setThreadName(name);
            try
            {
                fn();
            }
            catch (const std::exception &e)
            {
                LOGE("thread [%s] exited with exception: %s", name.c_str(), e.what());
            }
            catch (...)
            {
                LOGE("thread [%s] exited with unknown exception", name.c_str());
            }
        });
    }

    void join()
    {
        if (th_.joinable())
        {
            if (th_.get_id() == std::this_thread::get_id())
                th_.detach(); // 线程自己 join 自己会死锁, 只可能出现在异常路径
            else
                th_.join();
        }
    }

    bool joinable() const { return th_.joinable(); }

private:
    std::string name_;
    std::thread th_;
};

// 可被 running 标志打断的 sleep, 用于重连退避等长等待
inline void interruptibleSleep(int ms, const std::atomic<bool> &running)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

inline int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

#endif
