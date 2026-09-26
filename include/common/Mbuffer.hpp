#ifndef COMMON_MBUFFER_HPP
#define COMMON_MBUFFER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "opencv2/core/core.hpp"

struct FrameData
{
    cv::Mat img;       // BGR 图像
    uint64_t seq = 0;  // 写入序号, 从 1 开始递增
    int64_t pts = 0;   // 毫秒
    std::chrono::steady_clock::time_point ts; // 写入时间, 用于判断画面是否过期
};

// Mbuffer: "最新帧"缓冲槽(单槽覆盖写)
//  * 写入方(解码回调 / 推理线程)永不阻塞, 新帧直接覆盖旧帧;
//  * 读取方总是拿到最新的一帧, 下游处理不过来时自动丢弃中间帧, 保证实时性;
//  * 被覆盖而未被读取的帧计入 overwritten(), 用于定位瓶颈(下游太慢).
//
// 约定: 写入后的 cv::Mat 不再被写入方修改(每次写入新的 Mat),
//       读取方拿到的是引用计数的浅拷贝, 若要修改需要自己 clone.
class Mbuffer
{
public:
    Mbuffer() = default;
    Mbuffer(const Mbuffer &) = delete;
    Mbuffer &operator=(const Mbuffer &) = delete;

    void write(const cv::Mat &img, int64_t pts)
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (closed_)
                return;
            if (data_.seq != 0 && !consumed_)
                overwritten_++;
            data_.img = img;
            data_.pts = pts;
            data_.seq++;
            data_.ts = std::chrono::steady_clock::now();
            consumed_ = false;
        }
        cv_.notify_all();
    }

    // 等待比 after_seq 更新的帧; 超时或关闭返回 false
    bool waitNew(FrameData &out, uint64_t after_seq, int timeout_ms)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [&] { return closed_ || data_.seq > after_seq; }))
            return false;
        if (closed_)
            return false;
        out = data_;
        consumed_ = true;
        return true;
    }

    // 非阻塞获取最新帧(可能与上次相同), 还没有任何帧时返回 false
    bool peek(FrameData &out) const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (data_.seq == 0)
            return false;
        out = data_;
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return closed_;
    }

    uint64_t overwritten() const { return overwritten_.load(); }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    FrameData data_;
    bool consumed_ = true;
    bool closed_ = false;
    std::atomic<uint64_t> overwritten_{0};
};

#endif
