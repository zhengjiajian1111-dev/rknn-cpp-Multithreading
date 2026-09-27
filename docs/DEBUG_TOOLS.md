# 调试工具实战：GDB / strace / Valgrind

> 三个工具各配一个小例子，每个例子模拟本项目里真实会遇到的问题。文中的输出都是实际运行得到的，只删掉了无关的行。
> 项目整体的调试思路见 [LEARNING_GUIDE.md 第 12 章](LEARNING_GUIDE.md#12-调试方法)。

## 目录
1. [三个工具各管什么](#1-三个工具各管什么)
2. [常用命令速查](#2-常用命令速查)
3. [GDB：程序退出时卡死](#3-gdb程序退出时卡死)
4. [strace：进程无声无息地退出](#4-strace进程无声无息地退出)
5. [Valgrind：内存泄漏和越界](#5-valgrind内存泄漏和越界)
6. [用到项目上](#6-用到项目上)

---

## 1. 三个工具各管什么

| 工具 | 什么时候用 | 它告诉你 |
|---|---|---|
| **GDB** | 程序崩溃、卡死 | 每个线程**停在哪一行**，变量**现在是多少** |
| **strace** | 程序和系统打交道出错：文件打不开、网络不通、莫名其妙退出 | 程序调用了哪些**系统调用**，**返回了什么错误** |
| **Valgrind** | 内存泄漏、越界、使用已释放的内存 | **哪一行申请的内存没释放**，**哪一行越界** |

安装：`sudo apt install gdb strace valgrind`。GDB 和 Valgrind 需要程序带调试信息，编译时加 `-g -O0`（`-g` 带上行号信息，`-O0` 关掉优化，变量不会被优化掉）。

---

## 2. 常用命令速查

**GDB**

```
gdb ./prog                       启动程序调试
  break file.cc:20               在第 20 行打断点
  run / continue                 运行 / 继续运行
  next / step                    单步执行(next 不进函数, step 进函数)
  print 变量                      看变量的值
  bt                             看调用栈
gdb -p <进程号>                   附加到正在运行的进程(卡死时用)
  info threads                   列出所有线程
  thread apply all bt            打印所有线程的调用栈
  thread 2 / frame 6             切到 2 号线程 / 切到调用栈第 6 层
gdb ./prog core                  打开崩溃时生成的 core 文件, 看崩溃现场
```

**strace**

```
strace ./prog                         打印所有系统调用
strace -e trace=openat,connect ./prog 只看关心的系统调用
strace -f ./prog                      跟踪所有线程(多线程程序必加)
strace -p <进程号>                     附加到正在运行的进程
strace -c ./prog                      统计每种系统调用的次数和耗时
strace -tt -T -o log.txt ./prog       带时间戳和每次调用的耗时, 输出到文件
```

每一行的格式是：`系统调用(参数) = 返回值 错误码`。

**Valgrind**

```
valgrind --leak-check=full ./prog
```

重点看这几个关键词：

| 关键词 | 含义 |
|---|---|
| `Invalid read` / `Invalid write` | 越界，或者用了已释放的内存 |
| `definitely lost` | 确定泄漏 |
| `possibly lost` | 可能泄漏，也按泄漏查 |
| `still reachable` | 退出时还有指针指着，一般不算泄漏 |

程序在 Valgrind 下会慢 20～50 倍。

---

## 3. GDB：程序退出时卡死

模拟项目里"队列没关闭，退出时卡住"：修改停止标志后，忘了唤醒正在等待的消费者。

```cpp
// hang.cc: 生产者-消费者, 退出时忘了唤醒消费者 -> 程序卡在 join
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <queue>
#include <thread>

std::mutex mtx;
std::condition_variable cv;
std::queue<int> q;
bool stop = false;

void consumer()
{
    while (true)
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [] { return !q.empty() || stop; }); // 等数据或停止
        if (q.empty())
            break;
        int v = q.front();
        q.pop();
        lk.unlock();
        printf("consume %d\n", v);
    }
}

int main()
{
    std::thread t(consumer);
    for (int i = 0; i < 3; i++)
    {
        {
            std::lock_guard<std::mutex> lk(mtx);
            q.push(i);
        }
        cv.notify_one();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard<std::mutex> lk(mtx);
        stop = true;
    }
    // BUG: 忘了 cv.notify_all(), 消费者一直睡在 wait 上
    t.join();
    printf("exit\n");
}
```

**第 1 步：运行，现象是卡住**

```
$ g++ -g -O0 -pthread hang.cc -o hang
$ ./hang
consume 0
consume 1
consume 2
                  <- 卡在这里, 不打印 exit, 也不退出
```

**第 2 步：另开一个终端，附加上去，看所有线程停在哪**

```
$ gdb -p $(pidof hang)
(gdb) info threads
  Id   Target Id                              Frame
* 1    Thread 0x7f6e2349f740 (LWP 413) "hang" __futex_abstimed_wait_common64 (...)
  2    Thread 0x7f6e22dff6c0 (LWP 415) "hang" __futex_abstimed_wait_common64 (...)

(gdb) thread apply all bt

Thread 2 (Thread 0x7f6e22dff6c0 (LWP 415) "hang"):
#0  __futex_abstimed_wait_common64 (...)          <- 前几层是系统库内部, 跳过
#3  __pthread_cond_wait_common (...)              <- 在等条件变量
#4  ___pthread_cond_wait (...)
#5  std::condition_variable::wait<...> (...)
#6  consumer () at hang.cc:19                     <- 第一个"自己的代码": 消费者睡在 cv.wait 上

Thread 1 (Thread 0x7f6e2349f740 (LWP 413) "hang"):
#0  __futex_abstimed_wait_common64 (...)
#3  __pthread_clockjoin_ex (...)
#4  std::thread::join() ()
#5  main () at hang.cc:46                         <- 主线程在 join, 等消费者结束
```

**看调用栈的方法**：从上往下找，第一个出现你自己文件名的那一层就是关键。

**第 3 步：切到消费者线程，看变量**

```
(gdb) thread 2
(gdb) frame 6
#6  consumer () at hang.cc:19
19          cv.wait(lk, [] { return !q.empty() || stop; }); // 等数据或停止
(gdb) print stop
$1 = true
(gdb) print q
$2 = std::queue wrapping: std::deque with 0 elements
```

**结论**：`stop` 已经是 true，消费者却还睡着，说明改完标志后没有人叫醒它。

**修复**：在 `stop = true` 之后加 `cv.notify_all()`。

**对应到项目**：这就是项目里 `close()` 要 `notify_all`、退出时要"先 close 再 join"的原因（见 `src/app/App.cc` 的 `App::stop`）。

**程序崩溃时的用法**：

```
$ ulimit -c unlimited          允许生成 core 文件(只对当前终端有效)
$ ./prog                       崩溃后生成 core 文件
$ gdb ./prog core              core 文件的名字和位置由 /proc/sys/kernel/core_pattern 决定
(gdb) bt                       直接看到崩在哪一行
```

---

## 4. strace：进程无声无息地退出

模拟项目里"推流服务器一重启，程序直接没了，也没有任何日志"。

```cpp
// sigpipe.cc: 往对方已断开的连接写数据 -> 进程无声无息地退出
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

int main()
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv); // 一条连接的两端
    close(sv[1]);                            // 模拟: 推流服务器断开了连接
    fprintf(stderr, "sending frame...\n");
    write(sv[0], "frame", 5);                // 往已断开的连接写
    fprintf(stderr, "sent ok\n");            // 永远打印不出来
    return 0;
}
```

**第 1 步：运行，现象是程序直接没了**

```
$ g++ -g -O0 sigpipe.cc -o sigpipe
$ ./sigpipe
sending frame...
$ echo $?
141                          <- 退出码 141 = 128 + 13, 说明是被 13 号信号(SIGPIPE)杀死的
```

**第 2 步：用 strace 看它最后做了什么**

```
$ strace ./sigpipe
...                                                                   (前面是加载动态库, 跳过)
socketpair(AF_UNIX, SOCK_STREAM, 0, [3, 4]) = 0
close(4)                                = 0
write(2, "sending frame...\n", 17)      = 17
write(3, "frame", 5)                    = -1 EPIPE (Broken pipe)     <- 写失败: 对方已关闭
--- SIGPIPE {si_signo=SIGPIPE, si_code=SI_USER, ...} ---              <- 内核发来 SIGPIPE
+++ killed by SIGPIPE +++                                             <- 进程被杀死
```

**结论**：往已断开的连接写数据，内核会发 SIGPIPE，而它的默认处理就是直接杀死进程，程序自己来不及打任何日志。

**修复**：程序开头忽略 SIGPIPE，写操作就只返回错误，由程序自己处理：

```cpp
// sigpipe_fixed.cc: 忽略 SIGPIPE, 写失败时由程序自己处理
#include <csignal>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>

int main()
{
    signal(SIGPIPE, SIG_IGN); // 修复: 忽略 SIGPIPE, 让 write 返回错误
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv); // 一条连接的两端
    close(sv[1]);                            // 模拟: 推流服务器断开了连接
    fprintf(stderr, "sending frame...\n");
    if (write(sv[0], "frame", 5) < 0)
        perror("write failed, reconnect later"); // 由程序自己处理: 关闭连接、稍后重连
    return 0;
}
```

```
$ g++ -g -O0 sigpipe_fixed.cc -o sigpipe_fixed
$ ./sigpipe_fixed
sending frame...
write failed, reconnect later: Broken pipe
$ echo $?
0
```

**对应到项目**：`src/app/main.cc` 开头就是 `signal(SIGPIPE, SIG_IGN)`，推流写失败后关闭连接、等一会儿再重连。

**另外两个常用场景**：

```
# 找不到文件: 看它实际去哪个路径找模型/配置文件
$ strace -f -e trace=openat ./rknn_multi_stream -c config/app.ini 2>&1 | grep ENOENT

# 程序卡住但不确定卡在哪: 看它停在 connect / recvfrom(网络) 还是 futex(等锁或条件变量)
$ strace -f -p <进程号>
```

---

## 5. Valgrind：内存泄漏和越界

模拟项目里"推理出错时忘了释放输出缓冲"，再加一个数组越界。

```cpp
// leak.cc: 每帧申请输出缓冲, 出错分支忘了释放; 另有一处数组越界
#include <cstdio>

bool infer(int frame, unsigned char **out)
{
    *out = new unsigned char[1024 * 1024]; // 每帧 1MB 输出缓冲
    if (frame % 3 == 0)
        return false; // 模拟推理失败
    return true;
}

int main()
{
    for (int i = 0; i < 10; i++)
    {
        unsigned char *buf = nullptr;
        if (!infer(i, &buf))
        {
            printf("frame %d failed\n", i);
            continue; // BUG 1: 失败分支没有 delete[] buf
        }
        delete[] buf;
    }

    int *box = new int[4]; // x, y, w, h
    box[4] = 0;            // BUG 2: 越界写, 下标最大是 3
    delete[] box;
    return 0;
}
```

**第 1 步：直接运行，什么都看不出来**

```
$ g++ -g -O0 leak.cc -o leak
$ ./leak
frame 0 failed
frame 3 failed
frame 6 failed
frame 9 failed
$ echo $?
0                  <- 正常退出. 这类 bug 平时不报错, 只会让内存越涨越多, 或者偶尔崩溃
```

**第 2 步：用 Valgrind 跑**

```
$ valgrind --leak-check=full ./leak

== Invalid write of size 4
==    at 0x10929B: main (leak.cc:26)                          <- 第 26 行写越界
==  Address 0x512b190 is 0 bytes after a block of size 16 alloc'd
==    at 0x48485C3: operator new[](unsigned long) (...)
==    by 0x10928E: main (leak.cc:25)                          <- 写到了第 25 行申请的数组后面

== HEAP SUMMARY:
==     in use at exit: 4,194,304 bytes in 4 blocks
==   total heap usage: 13 allocs, 9 frees, 10,563,600 bytes allocated

== 1,048,576 bytes in 1 blocks are possibly lost in loss record 1 of 2
==    at 0x48485C3: operator new[](unsigned long) (...)
==    by 0x1091C5: infer(int, unsigned char**) (leak.cc:6)
==    by 0x109243: main (leak.cc:17)

== 3,145,728 bytes in 3 blocks are definitely lost in loss record 2 of 2
==    at 0x48485C3: operator new[](unsigned long) (...)
==    by 0x1091C5: infer(int, unsigned char**) (leak.cc:6)    <- 泄漏的内存是在这里申请的
==    by 0x109243: main (leak.cc:17)                          <- 从这里调用进去的

== LEAK SUMMARY:
==    definitely lost: 3,145,728 bytes in 3 blocks
==    indirectly lost: 0 bytes in 0 blocks
==      possibly lost: 1,048,576 bytes in 1 blocks
==    still reachable: 0 bytes in 0 blocks
== ERROR SUMMARY: 3 errors from 3 contexts (suppressed: 0 from 0)
```

**怎么读这份报告**

- **越界**：`0 bytes after a block of size 16`，意思是正好写到了 16 字节数组（4 个 int）末尾之后，也就是 `box[4]`。
- **泄漏**：确定泄漏 3 块，加可能泄漏 1 块，一共 4 块，每块 1MB，正好对应失败的第 0、3、6、9 帧。
- **找原因**：顺着调用栈 `leak.cc:6 ← leak.cc:17` 找到申请位置，再看调用方哪条路径没有释放，就能发现是失败分支少了 `delete[]`。

**第 3 步：修复后再跑一次**

修复：失败分支里加 `delete[] buf;`，越界的 `box[4]` 改成 `box[3]`，保存为 `leak_fixed.cc` 后重新编译。

```
$ valgrind --leak-check=full ./leak_fixed
==     in use at exit: 0 bytes in 0 blocks
== All heap blocks were freed -- no leaks are possible
== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

**对应到项目**

- `rknn_outputs_get` 之后必须 `rknn_outputs_release`；MPP 的缓冲区用完要马上还给解码器。
- **Valgrind 查不到硬件内存**：MPP、RGA、NPU 用的 DMA 内存不在普通的堆上。要看 `/proc/meminfo` 里的 CMA 用量是否一直在涨。
- 在整个项目上用 Valgrind 很慢，建议只开一路、跑一小段时间。平时更常用 ASan（编译加 `-fsanitize=address`），只慢 2 倍左右。

---

## 6. 用到项目上

**先编译带调试信息的版本**：项目默认是 Release 编译，没有行号信息。在构建目录里重新配置：

```
$ cd build/build_linux_aarch64
$ cmake ../.. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=RelWithDebInfo
$ make -j8 && make install
```

`RelWithDebInfo` 保留优化，同时带上行号信息，速度接近正式版本。个别变量可能显示 `<optimized out>`，要看变量时换成 `Debug`。

**项目里的线程都有名字**（`ch0-infer`、`ch1-dec` 之类，由 `WorkerThread` 设置），在 GDB 的 `info threads` 和 `top -H` 里能直接认出是哪一路的哪个线程。

**现象 → 工具 → 命令**

| 现象 | 用什么 | 命令 |
|---|---|---|
| 退出时卡住 / 运行中卡住 | GDB | `gdb -p <进程号>`，然后 `thread apply all bt` |
| 程序崩溃（段错误） | GDB | `ulimit -c unlimited` 后复现，`gdb ./rknn_multi_stream core`，然后 `bt` |
| 进程突然消失、没有日志 | strace | 先 `echo $?` 看退出码，再 `strace -f -o log.txt ./rknn_multi_stream -c ...` 看最后几行 |
| 模型或配置文件打不开 | strace | `strace -f -e trace=openat ... 2>&1 \| grep ENOENT` |
| 卡在网络上（拉流、推流、国标） | strace | `strace -f -p <进程号> -e trace=network` |
| 内存一直涨 | Valgrind / ASan | 只开一路短时间跑 `valgrind --leak-check=full`；同时看 CMA 用量 |
| 偶发崩溃、怀疑越界 | Valgrind / ASan | 同上，看 `Invalid read/write` |
