# 调试工具：GDB / strace / Valgrind

| 工具 | 最直观的理解 | 常用操作 |
|---|---|---|
| **GDB** | 暂停程序，查看某一行代码和变量 | 下断点、单步执行、查看调用栈 |
| **strace** | 查看程序向操作系统发出的请求 | 看文件打开、网络连接、信号是否正常 |
| **Valgrind** | 检查内存有没有用错、忘记释放 | 查越界、释放后使用、内存泄漏 |

每个工具配一个小例子，模拟项目里真实会遇到的问题。输出都是实际运行结果，只保留了关键行。编译时加 `-g -O0`（带行号、不做优化）。

---

## 1. GDB：程序退出时卡死

**场景**：程序处理完最后一帧，就是不退出。项目里对应"队列没关闭，线程一直在等数据"。

**关键代码**：

```cpp
stop = true;   // 改了停止标志
               // BUG: 忘了 cv.notify_all(), 消费者还睡在 cv.wait 上
t.join();      // 主线程等消费者结束, 永远等不到
```

**排查**：另开一个终端，附加到卡住的进程上：

```text
$ gdb -p $(pidof hang)
(gdb) thread apply all bt                  看每个线程停在哪

Thread 2:
#3  __pthread_cond_wait_common (...)       <- 在等条件变量
#6  consumer () at hang.cc:19              <- 第一个自己的函数: 消费者睡在 cv.wait
Thread 1:
#5  main () at hang.cc:46                  <- 主线程在 join

(gdb) thread 2                             切到消费者线程
(gdb) frame 6                              切到 consumer 那一层(编号以 bt 显示的为准)
(gdb) print stop
$1 = true                                  <- 已经要停了, 却还睡着: 没人叫醒它
```

**结论**：改完标志要 `cv.notify_all()`。项目里由 `close()` 负责叫醒，所以退出时"先 close 再 join"。

**其它常用**：

```text
gdb ./prog  →  break 文件:行  →  run  →  next / step  →  print 变量  →  continue
程序崩溃时输入 bt, 从上往下找第一个自己的函数, 就是崩溃位置
```

<details><summary>完整代码 hang.cc</summary>

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

</details>

---

## 2. strace：进程无声无息地退出

**场景**：推流服务器一重启，程序直接没了，没有任何日志。

**关键代码**：

```cpp
close(sv[1]);               // 对方断开了连接
write(sv[0], "frame", 5);   // 还往这条连接写
```

**排查**：

```text
$ ./sigpipe; echo $?
sending frame...
141                                              <- 141 = 128 + 13, 被 13 号信号(SIGPIPE)杀死

$ strace ./sigpipe
write(3, "frame", 5) = -1 EPIPE (Broken pipe)    <- 写失败: 对方已关闭
--- SIGPIPE {si_signo=SIGPIPE, ...} ---          <- 内核发来 SIGPIPE
+++ killed by SIGPIPE +++                        <- 进程被杀死
```

**结论**：SIGPIPE 的默认处理就是直接杀死进程。修复：程序开头加 `signal(SIGPIPE, SIG_IGN);`，写失败只返回错误，由程序自己重连。项目 `main.cc` 就是这么做的。

**其它常用**：

```text
-f                 跟踪所有线程(多线程程序必加)
-p <进程号>         附加到正在运行的进程
-e trace=openat    看打开了哪些文件, 找不到文件时 grep ENOENT
-e trace=network   看网络调用, 连不上时看错误码
```

> 拉流连不上时注意：FFmpeg 的 `connect` 先返回 `EINPROGRESS`（正在连，不是错误），真正的错误在紧跟着的 `getsockopt(... [ECONNREFUSED] ...)` 里。

<details><summary>完整代码 sigpipe.cc</summary>

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

</details>

---

## 3. Valgrind：内存泄漏和越界

**场景**：推理出错时忘了释放输出缓冲，另外有一处数组越界。平时运行完全正常，看不出问题。

**关键代码**：

```cpp
*out = new unsigned char[1024 * 1024];   // leak.cc:6   每帧申请 1MB
if (frame % 3 == 0) return false;        //             推理失败, 调用方 continue 时没 delete[]
int *box = new int[4];                   // leak.cc:25
box[4] = 0;                              // leak.cc:26  越界, 下标最大是 3
```

**排查**：

```text
$ valgrind --leak-check=full ./leak

Invalid write of size 4
   at main (leak.cc:26)                                    <- 越界的那一行
 Address ... is 0 bytes after a block of size 16 alloc'd   <- 写到了 16 字节数组的末尾之后

3,145,728 bytes in 3 blocks are definitely lost
   by infer(int, unsigned char**) (leak.cc:6)              <- 在这里申请
   by main (leak.cc:17)                                    <- 从这里调进去, 没有释放
```

**结论**：报告里还有 1 块 `possibly lost`，合起来 4 块，正好是失败的第 0、3、6、9 帧。修复后报告变成 `All heap blocks were freed` 和 `0 errors`。

**报告关键词**：

```text
definitely lost       确定泄漏
possibly lost         可能泄漏, 也按泄漏查
still reachable       退出时还有指针指着, 一般不算泄漏
Invalid read/write    越界, 或者用了已释放的内存
```

> 用到项目上：程序会慢 20～50 倍，只开一路短时间跑，并且要 Ctrl+C 正常退出才有报告。MPP、RKNN 的设备内存 Valgrind 看不到，要看 `/proc/meminfo` 里的 CMA 用量。

<details><summary>完整代码 leak.cc</summary>

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

</details>

---

## 项目里怎么用

先用 Debug 构建（默认 Release 没有行号）：`cmake ../.. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Debug`

| 现象 | 工具 | 命令 |
|---|---|---|
| 退出时或运行中卡住 | GDB | `gdb -p $(pidof rknn_multi_stream)`，然后 `thread apply all bt` |
| 段错误崩溃 | GDB | `gdb --args ./rknn_multi_stream -c config/app.ini`，`run` 到崩溃后 `bt` |
| 进程突然消失 | strace | 先 `echo $?` 看退出码，再 `strace -f -o log.txt ./rknn_multi_stream -c ...` 看最后几行 |
| 拉流连不上 | strace | `strace -f -e trace=network ./rknn_multi_stream -c ...`，看 `getsockopt` 里的错误码 |
| 内存一直涨 | Valgrind | 单路短时间跑 `valgrind --leak-check=full ...`，同时看 CMA 用量 |

项目里的线程都有名字（`ch0-dec`、`ch0-infer` 等），在 GDB 的 `info threads` 里能直接认出是哪一路。

---

**记住三个问题：GDB 查"代码执行到哪了"，strace 查"系统调用发生了什么"，Valgrind 查"内存有没有用错"。**
