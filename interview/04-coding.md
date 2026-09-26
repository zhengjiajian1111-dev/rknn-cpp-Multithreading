# 04 手撕代码

> 下面每道题的代码都用 `g++ -std=c++14 -Wall -Wextra -Werror` 编译并跑过测试用例；全部题目用 ASan + UBSan 跑过，并发题（1~6）额外用 ThreadSanitizer 跑过，均无报错。
> 练习方法：**先自己限时写（10~15 分钟），再对照**。写完要能口述：边界条件、复杂度、线程安全性。
> 题目按被考概率排序。和项目相关的题都标了 🔗，写完可以顺势讲项目。

| # | 题目 | 考点 |
|---|---|---|
| 1 | 有界阻塞队列（生产者-消费者）🔗 | mutex、两个条件变量、谓词、关闭 |
| 2 | 线程池 🔗 | 任务队列、packaged_task、future、优雅停止 |
| 3 | 两个线程交替打印 | 条件变量的最小应用 |
| 4 | 简易 shared_ptr | 引用计数、原子操作、拷贝交换 |
| 5 | 线程安全单例 🔗 | C++11 静态局部变量 |
| 6 | 最新帧缓冲（单槽覆盖）🔗 | 序号 + 条件变量 |
| 7 | memmove（处理内存重叠） | 指针、边界 |
| 8 | LRU 缓存 | 哈希表 + 双向链表 |
| 9 | IoU + NMS 🔗 | 排序、按类别抑制 |
| 10 | 33 位 PTS 打包进 5 字节 🔗 | 位操作 |
| 11 | 大端写入 RTP 头 🔗 | 字节序 |

---

## 题 1：有界阻塞队列 🔗

**要求**：多生产者多消费者；满了 `push` 阻塞，空了 `pop` 阻塞；支持 `close()` 让所有等待者退出。

```cpp
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

template <typename T>
class BoundedQueue
{
public:
    explicit BoundedQueue(size_t cap) : cap_(cap == 0 ? 1 : cap) {}

    // 满了就等; 队列已关闭返回 false
    bool push(T v)
    {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
        if (closed_)
            return false;
        q_.push(std::move(v));
        not_empty_.notify_one();
        return true;
    }

    // 空了就等; 队列已关闭且取空返回 false
    bool pop(T &out)
    {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty())
            return false;
        out = std::move(q_.front());
        q_.pop();
        not_full_.notify_one();
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        not_empty_.notify_all(); // 关闭是全局状态变化, 必须唤醒所有人
        not_full_.notify_all();
    }

private:
    const size_t cap_;
    bool closed_ = false;
    std::queue<T> q_;
    std::mutex m_;
    std::condition_variable not_empty_, not_full_;
};
```

**要能讲的点**：
- 为什么两个条件变量？生产者等"不满"、消费者等"不空"，分开后 `notify_one` 能精确唤醒对的那一方，避免唤醒同类线程又立刻睡回去。
- 为什么用谓词？防虚假唤醒和"被唤醒后条件又被别人改了"。
- `pop` 在关闭后仍能取完剩余元素（先判断 `q_.empty()`），这样关闭时不会丢数据。
- 🔗 项目里的版本（`include/common/BlockingQueue.hpp`）还带超时参数和 `tryPush`：实时流满了不阻塞，由调用方决定丢弃策略（丢整个 GOP）。

---

## 题 2：线程池 🔗

**要求**：固定线程数；`submit` 返回 `future` 拿结果；析构时执行完剩余任务再退出。

```cpp
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            workers_.emplace_back([this] { loop(); });
    }

    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_)
            t.join(); // 析构中 join, 避免 std::terminate
    }

    template <typename F>
    auto submit(F f) -> std::future<decltype(f())>
    {
        using R = decltype(f());
        // packaged_task 不可拷贝, 而 std::function 要求可拷贝, 所以用 shared_ptr 包一层
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(m_);
            if (stop_)
                throw std::runtime_error("submit on stopped pool");
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

private:
    void loop()
    {
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty())
                    return; // 先把剩余任务做完再退出
                job = std::move(tasks_.front());
                tasks_.pop();
            }
            job(); // 在锁外执行任务; 任务抛出的异常被 packaged_task 存进 future
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};
```

**要能讲的点**：
- 为什么用 `packaged_task`？它把返回值和**异常**都存进 future，调用方 `get()` 时拿到结果或重新抛出异常，线程本身不会因为异常而 terminate。
- 为什么任务在锁外执行？锁内只做出队，否则所有线程串行执行任务。
- 🔗 项目用的 `dpool::ThreadPool` 还支持按需创建线程、空闲 2 秒回收。线程数 = 模型实例数，保证不会死锁（02 A6）。

---

## 题 3：两个线程交替打印 1~n

**要求**：线程 A 打印奇数，线程 B 打印偶数，按顺序输出。（这里把输出写进 vector 方便测试，面试时换成 `printf` 即可。）

```cpp
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

void alternatePrint(int n, std::vector<int> &out)
{
    std::mutex m;
    std::condition_variable cv;
    int cur = 1;
    auto worker = [&](int parity) { // parity: 1 打印奇数, 0 打印偶数
        for (;;)
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return cur > n || cur % 2 == parity; });
            if (cur > n)
                return;
            out.push_back(cur++); // 面试时: printf("%d\n", cur++);
            cv.notify_all();      // 只有两个线程, notify_one 也可以; 但结束时必须能叫醒对方
        }
    };
    std::thread a(worker, 1), b(worker, 0);
    a.join();
    b.join();
}
```

**要能讲的点**：结束条件要写进谓词（`cur > n`），否则最后一个数打印完，另一个线程会永远等下去。

---

## 题 4：简易 shared_ptr

```cpp
#include <atomic>
#include <utility>

template <typename T>
class SharedPtr
{
public:
    explicit SharedPtr(T *p = nullptr) : ptr_(p), cnt_(p ? new std::atomic<long>(1) : nullptr) {}

    SharedPtr(const SharedPtr &o) : ptr_(o.ptr_), cnt_(o.cnt_)
    {
        if (cnt_)
            cnt_->fetch_add(1, std::memory_order_relaxed); // 增加计数不需要同步其它数据
    }

    SharedPtr(SharedPtr &&o) noexcept : ptr_(o.ptr_), cnt_(o.cnt_)
    {
        o.ptr_ = nullptr;
        o.cnt_ = nullptr;
    }

    // 拷贝交换: 同时处理拷贝赋值、移动赋值和自赋值
    SharedPtr &operator=(SharedPtr o) noexcept
    {
        swap(o);
        return *this;
    }

    ~SharedPtr() { release(); }

    void swap(SharedPtr &o) noexcept
    {
        std::swap(ptr_, o.ptr_);
        std::swap(cnt_, o.cnt_);
    }

    T *get() const { return ptr_; }
    T &operator*() const { return *ptr_; }
    T *operator->() const { return ptr_; }
    long use_count() const { return cnt_ ? cnt_->load() : 0; }

private:
    void release()
    {
        // acq_rel: 保证其它线程对对象的写, 在最后一个持有者 delete 之前都可见
        if (cnt_ && cnt_->fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            delete ptr_;
            delete cnt_;
        }
    }

    T *ptr_;
    std::atomic<long> *cnt_;
};
```

**要能讲的点**：
- 计数必须是原子的，多个线程可能同时拷贝/析构各自的 SharedPtr。
- 标准库的 `shared_ptr` 还有弱引用计数（给 `weak_ptr` 用）、自定义删除器、`make_shared` 把对象和控制块一次分配。
- "shared_ptr 线程安全吗"：计数安全，对象不安全，同一个 shared_ptr 实例被并发读写也不安全（03 6.2）。

---

## 题 5：线程安全单例 🔗

```cpp
class Singleton
{
public:
    static Singleton &instance()
    {
        static Singleton inst; // C++11 保证局部静态变量的初始化是线程安全的
        return inst;
    }
    Singleton(const Singleton &) = delete;
    Singleton &operator=(const Singleton &) = delete;

    int value() const { return value_; }

private:
    Singleton() = default;
    int value_ = 42;
};
```

**要能讲的点**：C++11 之前的"双重检查锁"容易因为指令重排拿到还没构造完的对象，需要内存屏障或原子指针。🔗 项目的 `PerfMonitor::instance()` 就是这种写法。

---

## 题 6：最新帧缓冲（单槽覆盖）🔗

**要求**：写入方永不阻塞，新数据覆盖旧数据；读者等待"比我上次拿到的更新"的数据，带超时。

```cpp
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

template <typename T>
class LatestSlot
{
public:
    void write(T v)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            val_ = std::move(v);
            ++seq_;
        }
        cv_.notify_all(); // 可能有多个读者
    }

    // seq 传入上次拿到的序号, 返回时更新为本次的序号
    bool waitNew(T &out, uint64_t &seq, int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return seq_ > seq; }))
            return false;
        out = val_;
        seq = seq_;
        return true;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    T val_{};
    uint64_t seq_ = 0;
};
```

**要能讲的点**：和阻塞队列的区别——队列保证不丢，这个保证"只要最新的"，下游慢时中间的数据自动被覆盖，延迟不会累积。读者可以通过序号的跳跃统计丢了多少帧。🔗 项目的 `Mbuffer` 还加了 `close()` 和覆盖计数。

---

## 题 7：memmove（处理内存重叠）

```cpp
#include <cstddef>
#include <cstdint>

void *myMemmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = static_cast<unsigned char *>(dst);
    const unsigned char *s = static_cast<const unsigned char *>(src);
    if (d == s || n == 0)
        return dst;
    // 用整数比较地址, 避免比较不相关指针的未定义行为
    uintptr_t di = reinterpret_cast<uintptr_t>(d), si = reinterpret_cast<uintptr_t>(s);
    if (di < si || di >= si + n)
    {
        // dst 在 src 前面, 或者两者不重叠: 从前往后拷
        while (n--)
            *d++ = *s++;
    }
    else
    {
        // dst 在 src 后面且重叠: 从后往前拷, 否则会覆盖还没读的源数据
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}
```

**要能讲的点**：`memcpy` 不保证处理重叠，`memmove` 保证。优化版本会按机器字长（8 字节）批量拷贝，并处理首尾不对齐的部分。

---

## 题 8：LRU 缓存

**要求**：`get`/`put` 都是 O(1)，容量满时淘汰最久未使用的。

```cpp
#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

class LRUCache
{
public:
    explicit LRUCache(size_t cap) : cap_(cap) {}

    bool get(int key, int &val)
    {
        auto it = map_.find(key);
        if (it == map_.end())
            return false;
        list_.splice(list_.begin(), list_, it->second); // 移到表头, O(1), 迭代器不失效
        val = it->second->second;
        return true;
    }

    void put(int key, int val)
    {
        if (cap_ == 0)
            return;
        auto it = map_.find(key);
        if (it != map_.end())
        {
            it->second->second = val;
            list_.splice(list_.begin(), list_, it->second);
            return;
        }
        if (list_.size() == cap_)
        {
            map_.erase(list_.back().first); // 表尾是最久未使用的
            list_.pop_back();
        }
        list_.emplace_front(key, val);
        map_[key] = list_.begin();
    }

private:
    size_t cap_;
    std::list<std::pair<int, int>> list_; // 表头最新, 表尾最旧
    std::unordered_map<int, std::list<std::pair<int, int>>::iterator> map_;
};
```

---

## 题 9：IoU + NMS 🔗

```cpp
#include <algorithm>
#include <vector>

struct Box
{
    float x1, y1, x2, y2, score;
    int cls;
};

float iou(const Box &a, const Box &b)
{
    float iw = std::max(0.f, std::min(a.x2, b.x2) - std::max(a.x1, b.x1));
    float ih = std::max(0.f, std::min(a.y2, b.y2) - std::max(a.y1, b.y1));
    float inter = iw * ih;
    float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni <= 0.f ? 0.f : inter / uni;
}

// 按类别的 NMS: 高分框只抑制与它 IoU > thresh 的"同类"低分框
std::vector<Box> nms(std::vector<Box> boxes, float thresh)
{
    std::sort(boxes.begin(), boxes.end(), [](const Box &a, const Box &b) { return a.score > b.score; });
    std::vector<bool> removed(boxes.size(), false);
    std::vector<Box> keep;
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        if (removed[i])
            continue;
        keep.push_back(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j)
        {
            if (!removed[j] && boxes[j].cls == boxes[i].cls && iou(boxes[i], boxes[j]) > thresh)
                removed[j] = true;
        }
    }
    return keep;
}
```

**要能讲的点**：复杂度 O(n²)；🔗 项目里原来的 NMS bug 是"排序后的位置"和"原始下标"混用、没检查被抑制框的类别（02 C6），手写时要注意同一个数组里下标含义要一致。

---

## 题 10：33 位 PTS 打包进 5 字节 🔗

**背景**：MPEG-2 PES 包头里的 PTS 是 33 位，按"4 位前缀 + 3 位 + marker | 15 位 + marker | 15 位 + marker"的格式放进 5 个字节（marker 位固定为 1，防止出现伪起始码）。

```
 byte0: 0 0 1 0 | PTS[32..30] | 1
 byte1: PTS[29..22]
 byte2: PTS[21..15]           | 1
 byte3: PTS[14..7]
 byte4: PTS[6..0]             | 1
```

```cpp
#include <cstdint>

void writePts(uint8_t *p, uint64_t pts)
{
    p[0] = (uint8_t)(0x20 | (((pts >> 30) & 0x07) << 1) | 0x01);
    p[1] = (uint8_t)(pts >> 22);
    p[2] = (uint8_t)((((pts >> 15) & 0x7F) << 1) | 0x01);
    p[3] = (uint8_t)(pts >> 7);
    p[4] = (uint8_t)(((pts & 0x7F) << 1) | 0x01);
}

uint64_t readPts(const uint8_t *p)
{
    return ((uint64_t)((p[0] >> 1) & 0x07) << 30) | ((uint64_t)p[1] << 22) | ((uint64_t)(p[2] >> 1) << 15) |
           ((uint64_t)p[3] << 7) | (uint64_t)(p[4] >> 1);
}
```

**要能讲的点**：PTS 单位是 90kHz（毫秒 × 90）；33 位大约 26.5 小时回绕一次。🔗 项目的 `src/output/gb28181/PsMuxer.cc` 里 PES 头和 PS 包头的 SCR 都是这种位域打包。

---

## 题 11：大端写入 RTP 头 🔗

**背景**：网络字节序是大端。不要用 `*(uint32_t*)p = htonl(x)`——`p` 可能没有对齐，在部分 ARM 平台上会出错；逐字节移位写法与主机字节序无关，也没有对齐问题。

```cpp
#include <cstdint>

inline void putBe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

inline void putBe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

inline bool isLittleEndian()
{
    uint16_t x = 1;
    return *reinterpret_cast<uint8_t *>(&x) == 1;
}

// RTP 固定头 12 字节: V=2 P=0 X=0 CC=0 | M | PT | seq | timestamp | SSRC
void writeRtpHeader(uint8_t *h, bool marker, uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    h[0] = 0x80;
    h[1] = (uint8_t)((marker ? 0x80 : 0x00) | (pt & 0x7F));
    putBe16(h + 2, seq);
    putBe32(h + 4, ts);
    putBe32(h + 8, ssrc);
}
```

**要能讲的点**：判断大小端的方法；`htonl`/`ntohl` 的作用；RK3588（ARM64）和 x86 都是小端。🔗 项目 `Gb28181Sink::write` 里的 RTP 头就是这样逐字节写的，TCP 模式前面再加 2 字节大端长度。
