#ifndef COMMON_BLOCKING_QUEUE_HPP
#define COMMON_BLOCKING_QUEUE_HPP

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

// 有界阻塞队列(生产者-消费者)
//  * push    : 队列满时阻塞(背压), 适合文件源: 不丢包, 由下游速度决定读取速度
//  * tryPush : 队列满时立即返回 false, 适合实时流: 由调用方决定丢弃策略
//  * close   : 唤醒所有等待者, 之后 push 失败、pop 取完剩余数据后返回 false,
//              保证退出时没有线程永久阻塞(线程能被 join)
template <typename T>
class BlockingQueue
{
public:
    explicit BlockingQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    BlockingQueue(const BlockingQueue &) = delete;
    BlockingQueue &operator=(const BlockingQueue &) = delete;

    // timeout_ms < 0 表示一直等待
    bool push(T value, int timeout_ms = -1)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        auto ready = [this] { return closed_ || queue_.size() < capacity_; };
        if (timeout_ms < 0)
            not_full_.wait(lock, ready);
        else if (!not_full_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready))
            return false;
        if (closed_)
            return false;
        queue_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    bool tryPush(T value)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (closed_ || queue_.size() >= capacity_)
            return false;
        queue_.push_back(std::move(value));
        not_empty_.notify_one();
        return true;
    }

    bool pop(T &out, int timeout_ms = -1)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        auto ready = [this] { return closed_ || !queue_.empty(); };
        if (timeout_ms < 0)
            not_empty_.wait(lock, ready);
        else if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready))
            return false;
        if (queue_.empty())
            return false; // closed 且已取空
        out = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // 清空队列, 返回丢弃的元素个数
    size_t clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        size_t n = queue_.size();
        queue_.clear();
        not_full_.notify_all();
        return n;
    }

    void close()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool closed() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return closed_;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    size_t capacity() const { return capacity_; }

private:
    const size_t capacity_;
    bool closed_ = false;
    std::deque<T> queue_;
    mutable std::mutex mtx_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
};

#endif
