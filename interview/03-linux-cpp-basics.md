# 03 Linux / C++ 基础八股（结合项目）

> 每题先给标准答案，再给 **🔗 结合项目**：面试时用项目里的例子回答，比背书有说服力得多。

## 目录
1. [多线程与同步](#1-多线程与同步)（必考）
2. [进程与进程间通信](#2-进程与进程间通信)
3. [网络编程](#3-网络编程)（必考）
4. [内存管理](#4-内存管理)
5. [信号](#5-信号)
6. [C++ 语言](#6-c-语言)
7. [编译、链接与工具](#7-编译链接与工具)
8. [部署与运维](#8-部署与运维)

---

## 1. 多线程与同步

### 1.1 互斥锁、条件变量、信号量、自旋锁、读写锁的区别？
- **互斥锁**：同一时刻只允许一个线程进入临界区，拿不到就睡眠。
- **条件变量**：配合互斥锁使用，让线程"等某个条件成立"，条件变化时由其他线程通知唤醒。
- **信号量**：带计数的同步原语，可以允许 N 个线程同时进入，也常用于"资源计数"。
- **自旋锁**：拿不到锁时忙等不睡眠，适合临界区极短、不想付出线程切换代价的场景（内核里常用，用户态慎用）。
- **读写锁**：读和读可以并发，写独占，适合读多写少。

🔗 **结合项目**：队列和 Mbuffer 用 `mutex + condition_variable`；模型实例池本质上是一个"计数信号量"（空闲实例数），我用 `mutex + condition_variable + 空闲列表` 实现，这样还能拿到具体是哪个实例。

### 1.2 条件变量为什么要配合 while（或谓词）使用？
- **虚假唤醒**：线程可能在没有被 notify 的情况下醒来（系统实现允许）。
- **被抢先**：被唤醒到重新拿到锁之间，条件可能已被别的线程改掉了（比如数据已被另一个消费者取走）。

所以醒来后必须重新检查条件：`while (!cond) cv.wait(lock);`，或者用带谓词的 `cv.wait(lock, pred)`，两者等价。

🔗 **结合项目**：所有等待都是 `wait_for(lock, 超时, 谓词)` 形式，谓词里除了数据条件还检查 `closed_`，保证退出时能被唤醒（`include/common/BlockingQueue.hpp`）。

### 1.3 notify_one 和 notify_all 的区别？什么时候用哪个？
- `notify_one` 唤醒一个等待者，`notify_all` 唤醒全部。
- 每次只多出一个"资源"（入队一个元素）时用 `notify_one`，避免惊群；状态发生全局变化（关闭、停止）时必须 `notify_all`，否则可能有线程永远醒不过来。
- 小细节：notify 可以在解锁之后调用，减少被唤醒的线程马上又阻塞在锁上的情况。

🔗 **结合项目**：队列入队/出队用 `notify_one`，`close()` 用 `notify_all`；Mbuffer 写入用 `notify_all`（可能有多个读者）。

### 1.4 死锁的四个必要条件？怎么预防？
- 互斥、占有并等待、不可剥夺、循环等待。
- 预防：固定加锁顺序（破坏循环等待）；一次性申请所有资源或持有时不再申请（破坏占有并等待）；`try_lock` 失败就释放已持有的锁（破坏不可剥夺）；用 `std::lock`/`std::scoped_lock` 同时锁多个互斥量；锁内不调用外部回调。

🔗 **结合项目**：线程池线程数 = 模型实例数，每个任务只持有一个实例且持有期间不再等别的资源，破坏"占有并等待"（02 A6）。

### 1.5 `std::atomic` 和 `volatile` 的区别？memory_order 了解吗？
- `volatile` 只保证每次都从内存读、不被编译器优化掉，**不保证原子性，也不保证多线程可见顺序**，不能用于线程同步（它是给硬件寄存器、信号处理用的）。
- `std::atomic` 保证操作原子，并通过 memory_order 控制可见顺序：
  - `relaxed`：只保证原子，不保证顺序，适合计数器；
  - `acquire/release`：release 之前的写，对 acquire 到同一变量的线程可见，适合"发布数据 + 标志位"；
  - `seq_cst`（默认）：全局一致顺序，最安全最慢。

🔗 **结合项目**：性能统计计数用 `fetch_add(relaxed)`（只要最终数对，不要求顺序）；停止标志用默认的 `seq_cst`；手写 shared_ptr 引用计数减到 0 时用 `acq_rel`（见 04 题 4）。

### 1.6 线程池的原理？线程数怎么定？
- 预先创建一组线程，从任务队列取任务执行，避免频繁创建销毁线程的开销，并限制并发度。
- 线程数：CPU 密集型 ≈ 核数；IO 密集型可以更多（核数 × (1 + 等待时间/计算时间)）；**受外部资源限制时，按资源数定**。

🔗 **结合项目**：推理线程池的线程数 = 模型实例数，因为真正的瓶颈是 NPU 实例，多出来的线程只会排队等实例。`future` 用来拿结果和传递异常。

### 1.7 线程和协程的区别？
- 线程由内核调度，切换要进内核，每个线程有独立的内核栈和较大的用户栈（默认 8MB 虚拟空间）；协程在用户态由程序自己调度，切换只是保存/恢复几个寄存器，非常轻量，但不能利用多核（除非配合多线程），遇到阻塞系统调用会卡住整个线程。

---

## 2. 进程与进程间通信

### 2.1 进程和线程的区别？
- 进程是资源分配单位（独立地址空间、文件描述符表），线程是调度单位（共享进程的地址空间和资源，各自有栈和寄存器）。
- 线程间通信方便（共享内存），但一个线程崩溃整个进程都崩；进程隔离好，但通信要靠 IPC，创建和切换开销更大。

🔗 **结合项目**：用多线程，因为各级之间要传每帧几 MB 的图像，共享内存零拷贝最方便。如果要求"推流模块崩了不影响检测"，可以把推流拆成独立进程，通过共享内存 + 信号量传帧。

### 2.2 进程间通信有哪些方式？
- 管道（匿名管道，父子进程）、命名管道 FIFO、消息队列、**共享内存**（最快，要配合信号量/互斥锁同步）、信号量、信号、**socket**（包括 Unix 域 socket，可以跨机器）、内存映射文件、DMA-BUF fd 传递（音视频/图形常用）。

### 2.3 僵尸进程和孤儿进程？
- 僵尸进程：子进程退出了，父进程没有 `wait`/`waitpid` 回收，进程表项还留着。解决：父进程回收，或者处理 `SIGCHLD`，或者设置 `SIGCHLD` 为 `SIG_IGN`。
- 孤儿进程：父进程先退出，子进程被 init/systemd 收养，无害。

### 2.4 fork 之后多线程程序有什么问题？
- `fork` 只复制调用它的那个线程；如果当时别的线程持有某把锁（比如 malloc 内部的锁），子进程里这把锁永远不会被释放，子进程一调用 malloc 就可能死锁。所以多线程程序里 fork 之后应该尽快 `exec`。

---

## 3. 网络编程

### 3.1 select / poll / epoll 的区别？
| | select | poll | epoll |
|---|---|---|---|
| fd 数量上限 | FD_SETSIZE（通常 1024） | 无 | 无 |
| 每次调用 | 要把整个 fd 集合从用户态拷贝到内核 | 同左 | 通过 `epoll_ctl` 注册一次，之后不用再拷贝 |
| 就绪检查 | 内核和用户都要遍历全部 fd，O(n) | O(n) | 内核用回调把就绪 fd 放进就绪链表，`epoll_wait` 只返回就绪的，O(就绪数) |
| 触发方式 | 水平触发 | 水平触发 | 水平触发(LT) / 边缘触发(ET) |

- **LT**：只要还有数据没读完，每次 `epoll_wait` 都会通知；**ET**：只在状态变化时通知一次，必须用非阻塞 fd，并循环读到 `EAGAIN`，否则剩下的数据不会再通知。

🔗 **结合项目**：GB28181 的 SIP 线程只监听 1 个 UDP socket，用 `poll` 带 100ms 超时，既能收报文又能定时处理注册刷新、心跳；fd 很少时 poll 和 epoll 没有区别。如果改成几十路拉流用事件驱动，就该用 epoll。

### 3.2 TCP 三次握手、四次挥手？TIME_WAIT 是什么？
- 三次握手：SYN → SYN+ACK → ACK。为什么要三次：双方都要确认对方能收也能发，并同步初始序号；两次的话，已失效的旧连接请求可能被误建立。
- 四次挥手：FIN → ACK →（对方数据发完）FIN → ACK。因为 TCP 全双工，两个方向要分别关闭。
- TIME_WAIT：**主动关闭方**在最后一个 ACK 之后等待 2MSL，保证最后的 ACK 丢了还能重发，并让网络里这个连接的旧报文全部过期，不影响新连接。大量短连接的服务端会堆积 TIME_WAIT。

### 3.3 SO_REUSEADDR 和 SO_REUSEPORT？
- `SO_REUSEADDR`（TCP）：允许绑定处于 TIME_WAIT 的端口，服务重启时不用等。
- 在 Linux 的 UDP 上，`SO_REUSEADDR` 允许多个 socket 绑定同一地址和端口，单播报文只会送给其中一个。
- `SO_REUSEPORT`：多个 socket 绑定同一端口，内核做负载均衡，常用于多进程/多线程服务器。

🔗 **结合项目**：GB28181 的 TCP 媒体监听 socket 设了 `SO_REUSEADDR`（快速复用端口）；SIP 的 UDP socket **故意不设**，这样端口被别的进程占用时 `bind` 会明确失败，我才能换下一个端口，而不是两个进程"静默"抢同一个端口（02 D10）。

### 3.4 非阻塞 connect 怎么实现超时？
1. 把 socket 设为 `O_NONBLOCK`；
2. `connect` 返回 -1 且 `errno == EINPROGRESS` 表示正在连接；
3. 用 `poll`/`select` 等待可写（`POLLOUT`），设置超时；
4. 可写后用 `getsockopt(SO_ERROR)` 取结果，0 表示成功；
5. 需要的话再把 socket 改回阻塞，并设 `SO_SNDTIMEO` 发送超时。

🔗 **结合项目**：GB28181 TCP 主动模式连接平台就是这么写的，超时 3 秒（`Gb28181Sink::connectMedia`）。

### 3.5 TCP 粘包是什么？怎么解决？
- TCP 是字节流，没有消息边界：发送方两次 `send` 的数据，接收方可能一次收到，也可能分几次收到。
- 解决：定长消息；特殊分隔符（如 HTTP/SIP 头部的 `\r\n\r\n`）；**长度前缀**（最常用）。

🔗 **结合项目**：GB28181 的 RTP over TCP 用 RFC 4571，每个 RTP 包前加 2 字节长度；SIP 报文用 `\r\n\r\n` 分头部和正文，再用 `Content-Length` 确定正文长度。

### 3.6 TCP 和 UDP 的区别？音视频为什么常用 UDP？
- TCP：面向连接、可靠、有序、有流量和拥塞控制，但丢包重传会造成队头阻塞，延迟抖动大。
- UDP：无连接、不可靠、保留消息边界、开销小、延迟低。
- 实时音视频"宁可丢一帧也不要卡住等重传"，所以常用 UDP（RTP），可靠性靠上层（FEC、NACK、关键帧请求）；需要穿透防火墙或网络很差时改用 TCP。

### 3.7 `send` 返回值要注意什么？
- 阻塞 socket 也可能只发出一部分（被信号打断、发送缓冲区不足），要循环发送直到发完；返回 -1 要看 `errno`：`EINTR` 重试，`EAGAIN` 非阻塞时缓冲区满，`EPIPE` 对端关闭（同时会有 SIGPIPE）。

🔗 **结合项目**：`Gb28181Sink::sendAll` 就是循环发送 + `EINTR` 重试 + `MSG_NOSIGNAL`。

---

## 4. 内存管理

### 4.1 虚拟内存是什么？为什么需要？
- 每个进程看到的是独立的虚拟地址空间，通过页表映射到物理内存（按页，一般 4KB）。好处：进程间隔离；程序可以使用比物理内存大的地址空间；按需分配（缺页时才真正分配物理页）；共享库、共享内存可以映射到多个进程。

### 4.2 进程的内存布局？
- 从低到高：代码段（text）、数据段（已初始化全局变量）、BSS（未初始化全局变量）、堆（向上增长）、mmap 区（共享库、大块 malloc、文件映射）、栈（向下增长）、内核空间。

### 4.3 malloc 是怎么工作的？
- glibc 的 malloc（ptmalloc）：小块内存从堆上分配（通过 `brk` 扩展堆），释放后放进空闲链表（bins）复用，不一定马上还给系统；大块内存（超过 `M_MMAP_THRESHOLD`，默认 128KB，会动态调整）直接用 `mmap` 分配，`free` 时 `munmap` 立即归还。
- 多线程时，每个线程可能用不同的分配区（arena），减少锁竞争，但会让内存"看起来"占用更多。

🔗 **结合项目**：每帧 1080p BGR 约 6MB，属于大块分配。排查"内存上涨"时要区分真泄漏和分配器缓存：可以用 `MALLOC_ARENA_MAX=2` 或调用 `malloc_trim(0)` 验证是不是 arena 缓存（学习指南 12.4.1）。

### 4.4 RSS、VSZ、PSS 分别是什么？
- VSZ：虚拟内存大小，包括所有映射（很多没有真正分配物理页），参考意义小。
- RSS：实际驻留在物理内存中的大小，包括共享库，**看泄漏主要看它的趋势**。
- PSS：按共享比例分摊后的大小，多进程统计总内存时更准确。
- 查看：`/proc/<pid>/status`（VmRSS、VmHWM 峰值）、`/proc/<pid>/smaps`（每段映射的明细）、`pmap -x`。

### 4.5 内存泄漏、内存越界、野指针分别怎么查？
- 泄漏：ASan/LeakSanitizer、valgrind memcheck、heaptrack。
- 越界、释放后使用、重复释放：ASan（最快）、valgrind。
- 预防：RAII、智能指针、容器代替裸数组、有界容器。

🔗 **结合项目**：见 02 E1/E2，重点说"硬件内存 ASan 看不到，要看 CMA/dma_buf 和对照申请释放表"，这是嵌入式音视频特有的点。

### 4.6 OOM killer 是什么？
- 系统内存耗尽时，内核按 oom_score 选一个进程杀掉。表现是进程突然消失、没有 core。排查：`dmesg | grep -i oom`。可以调 `/proc/<pid>/oom_score_adj` 保护关键进程。

---

## 5. 信号

### 5.1 信号处理函数里能做什么？
- 只能调用**异步信号安全**的函数（`write`、`_exit`、`sem_post` 等，见 `man 7 signal-safety`）。`printf`、`malloc`、加锁都不安全：信号可能在主流程持有 malloc 锁时到来，处理函数里再 malloc 就死锁了。
- 最佳实践：处理函数里只设置一个 `volatile sig_atomic_t` 或无锁的 `std::atomic<bool>` 标志，主循环检查标志后正常退出。

🔗 **结合项目**：`main.cc` 的 SIGINT/SIGTERM 处理函数只做 `g_quit = true`，主循环看到后走正常的退出流程。

### 5.2 `signal` 和 `sigaction` 的区别？
- `signal` 的语义在不同系统上不一致（处理后是否复位、被打断的系统调用是否自动重启）；`sigaction` 行为明确，可以设置信号屏蔽字和 `SA_RESTART` 等标志，推荐使用。

🔗 **结合项目**：SIGINT/SIGTERM 用 `sigaction`；SIGPIPE 只是忽略，用 `signal(SIGPIPE, SIG_IGN)` 足够。

### 5.3 常见信号？
- SIGINT（Ctrl+C）、SIGTERM（kill 默认，可捕获，用于优雅退出）、SIGKILL（不可捕获）、SIGSEGV（非法内存访问）、SIGABRT（abort，`std::terminate` 最终会触发）、**SIGPIPE**（写已关闭的连接，默认终止进程）、SIGCHLD（子进程状态变化）。

---

## 6. C++ 语言

### 6.1 RAII 是什么？
- 资源获取即初始化：在构造函数里获取资源，在析构函数里释放，靠对象生命周期自动管理，异常路径也不会泄漏。

🔗 **结合项目**：`WorkerThread`（析构 join）、`ModelManager::Lease`（析构归还实例）、`ScopedTimer`（析构记录耗时）、`std::unique_ptr` 管理各类对象。

### 6.2 shared_ptr、unique_ptr、weak_ptr？shared_ptr 线程安全吗？
- `unique_ptr`：独占所有权，零开销，只能移动。
- `shared_ptr`：共享所有权，引用计数；`weak_ptr` 不增加计数，用来打破循环引用、观察对象是否还活着。
- 线程安全：**控制块的引用计数是原子的**，多个线程各自拷贝/销毁不同的 `shared_ptr` 实例是安全的；但**同一个 shared_ptr 对象**被多线程同时读写不安全；**指向的对象**本身也没有任何保护。

🔗 **结合项目**：`VideoPacket` 用 `shared_ptr` 在线程间传递；`AVCodecParameters` 用 `shared_ptr` + 自定义删除器（`avcodec_parameters_free`）管理 C 库资源；标签列表用 `shared_ptr<const vector<string>>` 在同一模型的多个实例间共享（const 保证只读，天然线程安全）。

### 6.3 移动语义和完美转发？
- 移动语义：通过右值引用 `T&&` 和移动构造/赋值，把资源"搬走"而不是复制（比如 `std::vector` 移动只交换指针）。`std::move` 只是把左值转成右值引用，本身不移动任何东西。
- 完美转发：模板里用 `T&&`（万能引用）+ `std::forward<T>`，保持参数原本的左值/右值属性传给下一层。

🔗 **结合项目**：队列 `push(T value)` 内部 `std::move` 进容器；线程池 `submit` 用完美转发把可调用对象和参数传给 `std::bind`。

### 6.4 lambda 捕获有什么坑？
- 按引用捕获 `[&]` 或捕获 `this`，如果 lambda 比被捕获的对象活得久（比如交给另一个线程异步执行），就会访问悬空引用。
- 按值捕获指针也只是拷贝指针，对象本身还是可能先被销毁。

🔗 **结合项目**：`inferAll` 里任务按引用捕获了调用方栈上的图像和结果数组，所以必须等所有 future 完成才能返回；各 Worker 的线程 lambda 捕获了 `this`，所以析构函数里必须先 join 线程。

### 6.5 虚函数是怎么实现的？析构函数为什么要是虚的？
- 每个有虚函数的类有一张虚函数表，每个对象有一个虚表指针，调用虚函数时通过虚表间接调用，实现运行时多态。
- 通过基类指针删除派生类对象时，如果析构函数不是虚的，只会调用基类析构函数，派生类的资源泄漏（未定义行为）。

🔗 **结合项目**：`VideoDecoder`（MPP / FFmpeg）、`VideoEncoder`（MPP / FFmpeg）、`IStreamSink`（RTMP、RTSP / GB28181）都是抽象接口 + 虚析构，用工厂函数按配置和硬件可用性创建具体实现。

### 6.6 单例模式怎么写线程安全？
- C++11 起，函数内的静态局部变量初始化是线程安全的（"magic statics"），最简单的写法是 `static T& instance() { static T inst; return inst; }`（见 04 题 3）。双重检查锁在 C++11 之前容易写错（指令重排导致拿到未构造完成的对象）。

🔗 **结合项目**：`PerfMonitor::instance()` 就是这种写法。

### 6.7 `std::thread` 析构、异常、detach 的规则？
- `std::thread` 析构时如果仍 joinable（没有 join 也没有 detach），调用 `std::terminate`。
- 线程函数抛出的异常如果没被捕获，也会 `std::terminate`；要把异常传回调用方，用 `std::packaged_task`/`std::async`，异常会存进 future，`get()` 时重新抛出。
- detach 后线程独立运行，主线程无法再等它，进程退出时它可能还在访问已析构的对象。

🔗 **结合项目**：02 A7。

### 6.8 `std::function` 和函数指针的区别？
- 函数指针只能指向普通函数（或无捕获的 lambda）；`std::function` 可以装任何可调用对象（带捕获的 lambda、仿函数、bind 结果），代价是可能有堆分配和一次间接调用。

🔗 **结合项目**：解码器回调用 C 风格函数指针 + `userdata`（和 MPP 示例风格一致，零开销）；线程入口、拼接线程的状态回调用 `std::function`。

---

## 7. 编译、链接与工具

### 7.1 交叉编译要注意什么？
- 用目标平台的工具链（如 `aarch64-linux-gnu-g++`）；依赖库也要是目标架构的，通常通过 sysroot 提供头文件和库；CMake 用 toolchain 文件设置 `CMAKE_SYSTEM_NAME`、编译器和 `CMAKE_FIND_ROOT_PATH`，避免误找到主机上的库。

🔗 **结合项目**：`build-linux_RK3588.sh` 设置了 aarch64 编译器；RGA 和 RKNN 的 aarch64 库随仓库提供；PC 上调试时要关闭 RGA（否则会链接到 aarch64 版的 librga）。

### 7.2 动态库是怎么被找到的？rpath、runpath、LD_LIBRARY_PATH 的顺序？
- 查找顺序：`DT_RPATH`（只有在没有 `DT_RUNPATH` 时才生效）→ `LD_LIBRARY_PATH` → `DT_RUNPATH` → `/etc/ld.so.cache` → 默认目录（`/lib`、`/usr/lib`）。
- 较新的链接器默认生成 RUNPATH，所以 `LD_LIBRARY_PATH` 优先级更高。
- 排查工具：`ldd` 看依赖解析到了哪里，`readelf -d` 看 RPATH/RUNPATH，`LD_DEBUG=libs` 看加载过程。

🔗 **结合项目**：CMake 设置 `$ORIGIN/lib`，安装目录整体拷贝到任何位置都能找到 `lib/` 下的 librknnrt 和 librga。

### 7.3 静态库和动态库的区别？
- 静态库链接时拷进可执行文件，部署简单、无版本依赖问题，但体积大，库升级要重新链接；动态库运行时加载，多个进程共享一份物理内存，可以单独升级，但要处理版本兼容和查找路径。

### 7.4 常用调试工具？
| 工具 | 用途 |
|---|---|
| gdb | 断点、看栈、attach 运行中的进程、分析 core |
| strace | 看系统调用（卡在哪个 read/poll、打开了哪些文件、ioctl 失败） |
| ltrace | 看库函数调用 |
| top -H / htop | 按线程看 CPU |
| perf | CPU 热点、调用图 |
| valgrind / ASan / TSan | 内存错误、泄漏、数据竞争 |
| lsof / `ls /proc/<pid>/fd` | 打开的文件和 socket |
| ss / netstat | 连接状态（ESTABLISHED、TIME_WAIT） |
| tcpdump / Wireshark | 抓包分析协议 |

---

## 8. 部署与运维

### 8.1 怎么让程序开机自启、崩溃后自动重启？
用 systemd service：

```ini
# /etc/systemd/system/rknn-ms.service
[Unit]
Description=rknn multi stream
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/opt/rknn_multi_stream
ExecStart=/opt/rknn_multi_stream/rknn_multi_stream -c config/app.ini
Restart=on-failure
RestartSec=3
LimitCORE=infinity
KillSignal=SIGTERM
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
```

`systemctl enable --now rknn-ms`；日志用 `journalctl -u rknn-ms -f` 看。

🔗 **结合项目**：程序收到 SIGTERM 会走完整的优雅退出流程（发 BYE、注销、写 trailer），`TimeoutStopSec` 给它留出时间；无显示环境会自动切换到无窗口模式。

### 8.2 看门狗？
- 软件看门狗：systemd 的 `WatchdogSec` + 程序定期调用 `sd_notify("WATCHDOG=1")`，程序卡死不喂狗就被重启。可以在主循环或 perf 线程里检查"各路是否还在出帧"，都正常才喂狗（本项目还没做，是改进点）。
- 硬件看门狗：`/dev/watchdog`，系统级卡死时重启整机。
