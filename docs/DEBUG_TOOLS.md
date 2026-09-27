# 调试工具：GDB / strace / Valgrind

可以把它们理解成三种视角：

| 工具 | 最直观的理解 | 常用操作 |
|---|---|---|
| **GDB** | 暂停程序，查看某一行代码和变量 | 下断点、单步执行、查看调用栈 |
| **strace** | 查看程序向操作系统发出的请求 | 看文件打开、网络连接、设备访问是否成功 |
| **Valgrind** | 检查内存有没有用错、忘记释放 | 查越界、释放后使用、内存泄漏 |

下面每个工具对应项目里的一个例子。**命令在 RK3588 的 Linux 终端运行，进入程序安装目录后执行。** 文中标了"实测"的输出是在 PC 上用本项目程序实际跑出来的。

GDB 和 Valgrind 需要带调试信息的 Debug 构建（默认是 Release，没有行号）：

```bash
cd build/build_linux_aarch64
cmake ../.. -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Debug
make -j8 && make install
cd ../../install/rknn_multi_stream_Linux
```

---

## 1. GDB：暂停在解码回调，查看图像尺寸

**场景：解码后的画面异常（颜色错位、画面倾斜），想确认收到的宽度、高度、内存步长。**

启动调试器：

```bash
gdb --args ./rknn_multi_stream -c config/app.ini
```

然后在 GDB 中依次输入：

```gdb
break DecodeWorker::onFrame if ch_ == 0
run
print f.width
print f.height
print f.hor_stride
print f.ver_stride
next
continue
```

对应含义：

```text
break ... if ch_ == 0  → 在解码回调处暂停, 只停第 0 路(不加条件的话 4 个解码线程都会停)
run                    → 启动程序, 遇到断点停下
print                  → 查看变量
next                   → 执行下一行, 不进入被调用函数
continue               → 继续运行, 到下一次断点再停
```

1080p 视频经 MPP 解码，典型会看到：

```text
(gdb) print f.width
$1 = 1920
(gdb) print f.height
$2 = 1080
(gdb) print f.hor_stride
$3 = 1920
(gdb) print f.ver_stride
$4 = 1088
```

**重点看 `ver_stride`：垂直步长可能大于图像高度**（这里是 1088 > 1080）。NV12 的 UV 数据从 `hor_stride × ver_stride` 开始，而不是 `width × height`，按宽高去算就会颜色错位（见 `src/decode/DecodeWorker.cc` 的 CPU 回退路径）。

> PC 上走的是 FFmpeg 软解，`print f.format` 会显示 `BGR`，步长为 0；只有板子上走 MPP 才是 NV12 和上面的步长。

**两点提醒**

- 停在断点时，**整个程序的所有线程都暂停了**。拉实时流时对方可能因此断开，调试时用本地视频文件更方便。
- 项目里的线程都有名字，`info threads` 能直接看出是哪一路的哪个线程（实测）：

```text
(gdb) info threads
  Id   Target Id                             Frame
  1    Thread ... "main"      ...
  4    Thread ... "ch0-infer" ...
  8    Thread ... "ch0-dec"   ...
* 10   Thread ... "ch2-dec"   DecodeWorker::onFrame (...) at src/decode/DecodeWorker.cc:43
  12   Thread ... "ch0-demux" ...
```

**程序崩溃**：在 GDB 里运行到崩溃，然后输入：

```gdb
bt
```

它会显示"哪个函数调用了哪个函数"，从上往下找到第一个项目自己的函数，就是崩溃位置。

**程序卡住**：不用重启，直接附加到正在运行的进程上：

```bash
gdb -p $(pidof rknn_multi_stream)
```

```gdb
thread apply all bt
```

它会打印每个线程停在哪。退出时卡住最常见的是某个线程睡在条件变量上（`pthread_cond_wait`），说明它在等的队列没有被关闭、没人叫醒它。

---

## 2. strace：检查 RTSP 为什么连接不上

**场景：项目一直打印拉流失败、反复重连。**

```bash
strace -f -tt -e trace=network -o net.log \
  ./rknn_multi_stream -c config/app.ini
```

参数含义：

- `-f`：跟踪所有线程，这个项目必须加（拉流在 `chN-demux` 线程里）。
- `-tt`：记录时间。
- `-e trace=network`：只看网络相关的系统调用。
- `-o net.log`：保存结果。

打开 `net.log`，连接被拒绝时是这样的（实测）：

```text
18:17:10.313162 connect(3, {sa_family=AF_INET, sin_port=htons(8555), sin_addr=inet_addr("127.0.0.1")}, 16) = -1 EINPROGRESS (Operation now in progress)
18:17:10.314176 getsockopt(3, SOL_SOCKET, SO_ERROR, [ECONNREFUSED], [4]) = 0
18:17:10.825945 connect(3, ...) = -1 EINPROGRESS (Operation now in progress)     <- 0.5 秒后重试
18:17:11.835123 connect(3, ...) = -1 EINPROGRESS (Operation now in progress)     <- 1 秒后重试
18:17:13.836879 connect(3, ...) = -1 EINPROGRESS (Operation now in progress)     <- 2 秒后重试
```

**怎么读**：

- FFmpeg 用的是非阻塞连接：`connect` 先返回 `EINPROGRESS`，意思是"正在连"，**不是错误**。
- 真正的结果在紧跟着的 `getsockopt` 里：`ECONNREFUSED` 表示**连接被拒绝**，应检查目标端口是否有服务在监听，以及是否有主动拒绝连接的防火墙规则。
- 重试间隔 0.5 → 1 → 2 秒，就是项目 `StreamLoader` 的重连退避。
- 程序日志其实已经写了 `Connection refused`。strace 更大的用处是**看清实际连的是哪个 IP 和端口**：配置写错、域名解析成了别的地址，一眼就能看出来。

**地址不通**时又是另一种样子（实测，需要加上 `poll`：`-e trace=network,poll`）：

```text
connect(3, {... sin_port=htons(554), sin_addr=inet_addr("10.255.255.1")}, 16) = -1 EINPROGRESS (Operation now in progress)
poll([{fd=3, events=POLLOUT}], 1, 100) = 0 (Timeout)
poll([{fd=3, events=POLLOUT}], 1, 100) = 0 (Timeout)
...                                                    每 100ms 一次, 持续到 timeout_ms(2 秒)
```

对方完全不回应，最后日志里是 `Connection timed out`。应检查 IP、网段、网线和路由。

常见结果对照：

| 看到 | 含义 |
|---|---|
| `connect(...) = -1 EINPROGRESS` | 非阻塞连接"正在进行"，正常，看后面的结果 |
| `getsockopt(... [ECONNREFUSED] ...)` | 端口没有服务在监听，或者被拒绝 |
| 一直 `poll(...) = 0 (Timeout)` | 对方不回应：地址错、网络不通、被防火墙丢包 |
| `bind(...) = -1 EADDRINUSE` | 本地端口已被占用。项目的国标模块会自动换下一个端口，出现一次不一定是问题 |
| `recvfrom(...) = -1 EAGAIN` | 当前没有数据可读，非阻塞读的正常结果，未必是故障 |

**关键：看返回值和错误码，并结合后续调用判断，不是看到 `-1` 就认定有问题。**

---

## 3. Valgrind：检查反复重连有没有泄漏内存

**场景：反复断开、恢复视频源后，程序内存不断上涨，怀疑旧的 FFmpeg 资源没有释放。**

Valgrind 下程序会慢 20～50 倍，先准备一份**只有一路、低帧率**的配置（比如只留 `[source0]`，`[infer] target_fps = 5`），再运行：

```bash
valgrind --tool=memcheck --leak-check=full \
  --log-file=mem.log \
  ./rknn_multi_stream -c config/one.ini
```

运行后，让视频源经历几次断线重连，再按 **Ctrl+C 正常退出**，查看 `mem.log`。一定要正常退出，`kill -9` 杀掉的话没有报告。

一路 RTSP、断开恢复 2 次后的结果（实测）：

```text
== HEAP SUMMARY:
==     in use at exit: 55,832 bytes in 302 blocks
==   total heap usage: 168,546 allocs, 168,244 frees, 932,966,051 bytes allocated
== LEAK SUMMARY:
==    definitely lost: 0 bytes in 0 blocks
==    indirectly lost: 0 bytes in 0 blocks
==      possibly lost: 0 bytes in 0 blocks
==    still reachable: 53,816 bytes in 281 blocks
== ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

**怎么读**：

- `definitely lost: 0`，没有确定泄漏；`ERROR SUMMARY: 0`，没有非法读写。
- `still reachable` 约 53KB，是 FFmpeg、OpenCV 等库初始化后一直保留的全局数据，退出时仍有指针指着，**不是泄漏**。
- **判断技巧**：分别断开恢复 2 次和 10 次，对比结果。真正的泄漏会随重连次数增长，库的全局数据不会。

如果有泄漏，报告会像这样（**示例**）：

```text
1,024 bytes in 1 blocks are definitely lost in loss record 12 of 30
    at malloc (...)
    by av_malloc (...)
    by StreamLoader::openInput() (StreamLoader.cc:...)
```

意思是：

> 有 1024 字节已经找不到任何指针指向它，属于确定泄漏。沿报告中的调用栈找到分配位置，再检查对应的释放路径。

在项目里，重点对照：

```text
StreamLoader::openInput()   → 打开输入、分配资源
StreamLoader::closeInput()  → 关闭输入、释放资源
```

其它几类常见报告：

```text
Invalid read/write       → 非法读写, 可能越界或访问已释放的内存
possibly lost            → 只找到指向内存中间的指针, 也按泄漏排查
still reachable          → 退出时仍有指针持有内存, 不等同于确定泄漏
```

**Valgrind 的局限**：MPP、RGA、RKNN 使用的设备内存（DMA 内存）不在普通的堆上，Valgrind 看不到。这部分要看 `/proc/meminfo` 里的 CMA 用量是否一直在涨。板子上这几个库自己也可能报出不少记录，重点看调用栈里有项目自己函数的那几条。

---

**记住这三个问题即可：GDB 查"代码执行到哪了"，strace 查"系统调用发生了什么"，Valgrind 查"内存有没有用错"。**
