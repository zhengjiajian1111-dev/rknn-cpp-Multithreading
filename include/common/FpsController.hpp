#ifndef COMMON_FPS_CONTROLLER_HPP
#define COMMON_FPS_CONTROLLER_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

// 目标帧率控制
//  * accept(): 丢帧式, 用于"来多少帧不可控"的场景(如推理线程), 超出目标帧率的帧直接丢弃;
//  * wait()  : 节拍式, 用于"按固定帧率产出"的场景(如拼接/推流线程), 睡到下一个时间点.
// 两者都以绝对时间点累加周期, 避免 sleep 误差累积造成的帧率漂移;
// 落后超过一个周期时重新对齐, 避免处理变慢后"追帧"导致的突发.
class FpsController
{
public:
    using Clock = std::chrono::steady_clock;

    explicit FpsController(double fps = 0) { setFps(fps); }

    void setFps(double fps)
    {
        if (fps > 0)
            period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
        else
            period_ = Clock::duration::zero();
        started_ = false;
    }

    bool enabled() const { return period_ > Clock::duration::zero(); }

    bool accept(Clock::time_point now = Clock::now())
    {
        if (!enabled())
            return true;
        if (!started_)
        {
            started_ = true;
            next_ = now + period_;
            return true;
        }
        // 允许 20% 的抖动, 避免输入帧率刚好等于目标帧率时因时间抖动误丢帧
        if (now + period_ / 5 < next_)
            return false;
        next_ += period_;
        if (next_ < now)
            next_ = now + period_;
        return true;
    }

    // running 为 false 时提前返回, 保证线程能及时退出
    void wait(const std::atomic<bool> *running = nullptr)
    {
        if (!enabled())
            return;
        auto now = Clock::now();
        if (!started_)
        {
            started_ = true;
            next_ = now;
        }
        next_ += period_;
        if (next_ < now)
        {
            next_ = now;
            return;
        }
        while (Clock::now() < next_)
        {
            if (running && !running->load())
                return;
            auto remain = next_ - Clock::now();
            std::this_thread::sleep_for(std::min<Clock::duration>(remain, std::chrono::milliseconds(50)));
        }
    }

private:
    Clock::duration period_{};
    Clock::time_point next_{};
    bool started_ = false;
};

#endif
