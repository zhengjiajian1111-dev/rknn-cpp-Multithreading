# 项目学习指南（原理与流程图解）

> 目标：搞懂这个项目**怎么工作、为什么这么设计、出问题怎么查**，达到面试能讲清楚的程度。
> 读法：先看第 1 章建立整体印象 → 第 2 章核心原理 → 第 3~9 章沿数据流逐段看 → 第 10 章启动/退出 → 第 11 章难点 → 第 12 章调试。
> 约定：图里方框内只用英文（保证对齐），中文注释写在框外。面试问答见 [INTERVIEW_GUIDE.md](INTERVIEW_GUIDE.md)。

## 目录

- [1. 全局：一条完整链路](#1-全局一条完整链路)
- [2. 核心原理](#2-核心原理)
- [3. 链路 s1：拉流](#3-链路-s1拉流)
- [4. 链路 s2：MPP 硬件解码](#4-链路-s2mpp-硬件解码)
- [5. 链路 s3：NV12 转 BGR](#5-链路-s3nv12-转-bgr)
- [6. 链路 s4：NPU 推理](#6-链路-s4npu-推理)
- [7. 链路 s5：多模型结果融合](#7-链路-s5多模型结果融合)
- [8. 链路 s6：拼接与推流](#8-链路-s6拼接与推流)
- [9. 链路 s6'：GB28181 国标接入](#9-链路-s6gb28181-国标接入)
- [10. 启动与退出顺序](#10-启动与退出顺序)
- [11. 项目难点与应对](#11-项目难点与应对)
- [12. 调试方法](#12-调试方法)
- [13. 学习路线与练习](#13-学习路线与练习)

---

## 1. 全局：一条完整链路

### 1.1 一句话

在 RK3588 上同时接入 4 路视频，用 **MPP 硬解 → RGA 转换 → RKNN 推理（可多模型并行）→ 多模型结果融合**，把结果**拼接显示**，并按配置推到 **RTMP / RTSP / GB28181**。

### 1.2 全链路图

```
 输入层    [ MP4 file ]  [ RTSP camera ]  [ RTMP / HTTP-FLV ]  [ /dev/video0 ]         x N 路(默认 4 路)
                \              |                  |                 /
                 v             v                  v                v
 s1 拉流   +------------------------------------------------------------+
           | StreamLoader                        thread: chN-demux      |   avformat_open_input / av_read_frame
           |   AVCC -> Annex-B (h264_mp4toannexb / hevc_mp4toannexb)    |   超时中断 + 指数退避重连
           |   live: tryPush + GOP drop    file: push (backpressure)    |   实时流丢 GOP / 文件流背压
           +------------------------------------------------------------+
                                  |  BlockingQueue<VideoPacketPtr>   有界队列, 默认容量 64
                                  v
 s2 解码   +------------------------------------------------------------+
           | DecodeWorker -> MppDecoder          thread: chN-dec        |   decode_put_packet / decode_get_frame
           |   fallback: FfmpegDecoder (libavcodec)                     |   info_change -> DRM buffer group
           +------------------------------------------------------------+
                                  |  NV12  (hor_stride x ver_stride, DMA-BUF fd)
                                  v
 s3 转换   +------------------------------------------------------------+
           | mpp_decoder_cb -> onFrame                                  |   RGA imcvtcolor (fd 零拷贝)
           |   fallback: strip stride + cv::cvtColor(YUV2BGR_NV12)      |   CPU 回退: 去 stride 拷贝 + cvtColor
           +------------------------------------------------------------+
                                  |  Mbuffer decoded[i]   最新帧槽(覆盖写)
                                  v
 s4 推理   +--------------------------------------+   submit    +-------------------------------+
           | InferWorker        thread: chN-infer |------------>| ModelManager                  |
           |   FpsController.accept(target_fps)   |             |   dpool::ThreadPool           |
           |   infer_interval / result reuse      |<------------|   model0: inst0 inst1 inst2   |
           +--------------------------------------+   future    |           core0 core1 core2   |
                                  |  g1..gN                      |   model1: inst0 inst1 ...     |
                                  v  (detect_result_group_t)     +-------------------------------+
 s5 融合   +------------------------------------------------------------+
           | DetectionFusion::fuseDetections -> drawFusedDetections     |   关联 -> WBF/noisy-OR/NMS -> 投票 -> NMS
           +------------------------------------------------------------+
                                  |  Mbuffer images[i]   画好框的结果帧
                 +----------------+-------------------+
                 v                                    v
 s6 输出  +------------------------+        +--------------------------------------+
          | Compositor             | mosaic | StreamingMgr        thread: pushN    |
          |   combineImage (2x2)   |------->|   clone + OSD -> VideoEncoder (MPP)  |
          +-----------+------------+ Mbuffer|   -> IStreamSink                     |
                      |                     +-------+-------------+-------------+--+
                      v                             |             |             |
             main: cv::imshow                     RTMP          RTSP        GB28181 (+ thread: pushN-sip)
```

### 1.3 线程地图

```
 main ─────────── 解析配置 / 信号处理 / cv::imshow(只在主线程调用 GUI)
 perf ─────────── 每秒汇总统计, 每 perf_interval 秒打印瓶颈报告
 ch0-demux ─┐
 ch0-dec    ├──── 第 0 路: 拉流 -> 解码 -> 推理  (每路 3 个线程, 4 路共 12 个)
 ch0-infer ─┘
 ...
 推理线程池 ────── dpool::ThreadPool, 线程数 = 所有模型实例数之和(多模型时才用)
 mosaic ───────── 固定帧率拼接
 push0 / push1 ── 每个启用的 [pushN] 一个: OSD + 编码 + 发送
 push2-sip ────── GB28181 推流项额外的 SIP 信令线程
```

每路是一条独立的流水线，4 路同时跑、互不等待；4 个推理线程共享 NPU。排查问题时用 `top -H -p <进程号>` 能看到上面这些线程名和各自的 CPU 占用。

### 1.4 一帧数据的一生（以 1080p 输入、640x640 模型为例）

| 阶段 | 数据形态 | 大致大小 | 所在容器 |
|---|---|---|---|
| s1 读包 | H.264 Annex-B 一帧 | 几 KB ~ 几百 KB(关键帧大) | `VideoPacket` → `BlockingQueue` |
| s2 解码 | NV12, 1920x1088(stride 对齐) | 1920×1088×1.5 ≈ 3.1 MB | MPP DRM buffer(DMA-BUF) |
| s3 转换 | BGR, 1920x1080 | ≈ 6.2 MB | `cv::Mat` → `Mbuffer decoded[i]` |
| s4 预处理 | RGB, 640x640, letterbox | ≈ 1.2 MB | `RknnLite::input_`(复用) |
| s4 推理输出 | int8, 255×80×80 + 255×40×40 + 255×20×20 | ≈ 2.1 MB | `rknn_output[3]` |
| s4 后处理 | 检测框(每模型 ≤ 64 个) | < 5 KB | `detect_result_group_t` |
| s5 融合 | `FusedDetection` 列表 + 画框后的 BGR | ≈ 6.2 MB | `Mbuffer images[i]` |
| s6 拼接 | BGR 1920x1080 马赛克 | ≈ 6.2 MB | `Mbuffer mosaic` |
| s6 编码 | H.264 一帧 | 几 KB ~ 百 KB | `EncodedPacket` |
| s6 GB28181 | PS 封装 → RTP 分片 | 每片 ≤ 1412 B | UDP 包 / TCP 流 |

> 体会一下：压缩包 KB 级，解码后 MB 级。**所以"拉流→解码"之间放大容量的队列没问题，"解码之后"只能放单槽最新帧**，否则内存和延迟都会爆。

### 1.5 目录与模块对应

```
config/app.ini                   所有可调参数(多路/多模型/融合/推流开关)
include/ src/
  app/        App.cc, main.cc    总装, 初始化/退出顺序, 信号, 命令行
  common/                        地基: Config, Logger, BlockingQueue, Mbuffer, FpsController,
                                 WorkerThread, PerfMonitor, RgaUtils, ThreadPool
  stream/     StreamLoader       s1 拉流
  decode/     MppDecoder, FfmpegDecoder, DecodeWorker      s2/s3
  infer/      rknn_lite, ModelManager, InferWorker, pre/postprocess   s4
  fusion/     DetectionFusion    s5
  output/     Compositor, StreamingMgr, MppEncoder, FfmpegEncoder,
              FfmpegPushSink, Gb28181Sink, gb28181/{SipMessage, PsMuxer, Md5}   s6
```

### 1.6 技术栈

| 技术 | 作用 | 硬件不可用时 |
|---|---|---|
| FFmpeg | 拉流（读 RTSP/文件）、推流（RTMP/RTSP） | 必需 |
| MPP | Rockchip 硬件视频编解码 | 回退 FFmpeg 软件编解码 |
| RGA | Rockchip 硬件 2D 图像处理（颜色转换、缩放） | 回退 OpenCV |
| RKNN | Rockchip NPU 推理 | 必需 |
| OpenCV | 画框、显示、软件兜底 | 必需 |
| C++ 多线程 | 线程、互斥锁、条件变量、原子变量、线程池 | — |

---

## 2. 核心原理

### 2.1 生产者-消费者：两种缓冲区

上一级生产数据、下一级消费数据，中间放一个缓冲区，两边就能按各自的速度工作。项目里有两种缓冲区：

```
 BlockingQueue<T> (有界 FIFO, include/common/BlockingQueue.hpp)

   producer --push()--> [ p5 | p4 | p3 | p2 | p1 ] --pop()--> consumer
                         |<------ capacity ----->|
   push()    : 满了就等(背压)               -> 文件源用, 不丢数据
   tryPush() : 满了立即返回 false          -> 实时流用, 由调用方决定怎么丢
   close()   : 唤醒所有等待者, 之后 push 失败, pop 取完剩余后返回 false
```

```
 Mbuffer (单槽最新帧, include/common/Mbuffer.hpp)

   writer: write(f1)  write(f2)  write(f3)            <- 永不阻塞, 新帧直接覆盖
                 \        \         \
   slot:        [f1] ->  [f2]  ->  [f3]   seq: 1 -> 2 -> 3
                           ^
   reader: waitNew(after_seq=1) 拿到 f2; 若 f2 还没读就被 f3 覆盖 -> overwritten++
   peek(): 非阻塞拿当前最新(拼接/推流线程用, 可能重复拿到同一帧)
```

| | BlockingQueue | Mbuffer |
|---|---|---|
| 语义 | 不丢(或由调用方丢) | 只保留最新, 自动丢旧 |
| 适合 | 压缩码流(丢了会花屏) | 解码后的图像(实时优先) |
| 本项目位置 | 拉流 → 解码 | 解码 → 推理 → 拼接/推流 |
| 满了怎么办 | 等待 / tryPush 失败 | 覆盖, 计数 overwritten |

**cv::Mat 跨线程的约定**：写入 Mbuffer 的 Mat 之后不再被写入方修改(每帧 new 一个新 Mat)，读者拿到的是引用计数浅拷贝。需要修改的读者(例如推流线程要叠加时间戳)必须自己 `clone()`。

一句话记住：**码流不能随便丢，图像只要最新的。**

### 2.2 条件变量的正确用法

两种缓冲区都是"互斥锁 + 条件变量"实现的，要点：
- **互斥锁**保护数据，同一时刻只有一个线程能改；
- **条件变量**让等待的线程睡眠，数据来了再叫醒，不浪费 CPU；
- 醒来后**必须重新检查条件**：可能是虚假唤醒，也可能条件已被别的线程改掉；
- 所有等待都**带超时或能被 `close()` 叫醒**，保证退出时不会卡死；
- 锁里只做很轻的操作（入队出队、引用计数），不在锁里做编码、网络 IO。

### 2.3 资源竞争：模型实例池

**竞争在哪**：4 个推理线程，但 NPU 只有 3 个核；同一个模型实例（`rknn_context`）同一时刻只能被一个线程使用。

**怎么解决**：实例池，像借东西一样：

```
 空闲列表: [实例0(核0), 实例1(核1), 实例2(核2)]      <- 一把锁保护

 前处理: 在自己的工作区里做, 不占实例
 借: 加锁 -> 有空闲就拿走一个, 没有就在条件变量上睡着等 -> 解锁
 用: 用这个实例跑 NPU (此时只有我在用), 输出直接写进自己的工作区
 还: 加锁 -> 放回列表 -> 叫醒一个在等的线程 -> 解锁      (写在析构函数里, 出错也一定会还)
 后处理: 在自己的工作区里做, 不占实例
```

- **路和路之间**：没有固定顺序，谁先借到谁先算。
- **同一路内部**：严格按时间顺序。一个推理线程算完一帧才拿下一帧，跟不上就跳过中间帧、拿最新的。

单模型时 4 路抢 3 个实例的时间线（每次推理约 12ms）：

```
 时间(ms)   0          12         24         36
 核0:      [ch0 帧1 ] [ch3 帧1 ] [ch0 帧2 ] ...
 核1:      [ch1 帧1 ] [ch1 帧2 ] [ch3 帧2 ] ...
 核2:      [ch2 帧1 ] [ch2 帧2 ] [ch1 帧3 ] ...
 ch3:       等实例...   ^ ch0 归还后借到实例0
```

同一时刻最多 3 路在真正推理，第 4 路短暂排队。

**只在 NPU 那一步借实例**：一次推理分三步，前处理和后处理是 CPU/RGA 在干活，只有中间一步用 NPU。如果三步都占着实例，实例在做前后处理时，它绑定的核只能空等。用一组板端数据（前处理约 12ms、NPU 约 21ms、后处理约 3ms）来看：

```
 改之前: 三步都占着实例 (实例0 绑核0)
 实例0:  [前处理 12][  NPU 21  ][后 3][前处理 12][  NPU 21  ][后 3]
 核0:    [  空闲   ][   忙     ][空闲][  空闲   ][   忙     ][空闲]    核只忙 21/36 ≈ 58%

 改之后: 前后处理在各路自己的工作区里做, 只在 NPU 这一步借实例
 时间(ms)  0        12          33  36        48
 ch0:      [前处理 ][  NPU 21   ][后][前处理  ]   ...
 ch3:        [前处理 ]  等核0... [  NPU 21   ][后]
 核0:               [ ch0 的 NPU ][ ch3 的 NPU ]   ...    ch0 做后处理和下一帧前处理时, 核已经在跑 ch3
```

- 这样三个核的上限从每秒约 3 × 1000/36 ≈ 83 次，提高到约 3 × 1000/21 ≈ 143 次，排队借实例的时间也变短。
- 但**每一路自己的帧率**仍然是 1 / (前处理 + NPU + 后处理 + 等新帧)，因为一路一次只处理一帧。想让每路更快，还要把前处理本身压下来（比如 12ms 的 RGA 换成几毫秒的 OpenCV，以 `<model>.pre` 实测为准）。
- 实现：`RknnLite` 拆成 `prepare / run / decode` 三步，只有 `run` 在借到的实例上执行；输出用 `is_prealloc` 直接写进调用方的工作区，所以归还实例后还能做后处理。

### 2.4 线程池：只在多模型时用

**线程池**：提前建好几个线程守着一个任务队列，有任务就取出来执行，结果放进 `future`（相当于取餐凭证），提交的人凭它等结果。

- **单模型**：不用线程池，推理线程自己借实例、自己推理。
- **多模型**：同一帧要跑 A、B 两个模型，推理线程把两个任务提交到线程池，两个模型在不同 NPU 核上同时算：

```
 单线程串行:   |--model0 12ms--|--model1 12ms--|              = 24ms
 线程池并发:   |--model0 12ms--|                                = ~13ms
               |--model1 12ms--|
               推理线程拿着两张"凭证"(future)等两个结果都完成, 再做融合
```

**为什么不会死锁**：线程池线程数 = 模型实例总数；每个任务只借一个实例，借到后一定能算完归还，不会"拿着一个又等另一个"（破坏了死锁的"占有并等待"条件）。

### 2.5 三层并行

```
 1. 路级并行:     4 条流水线同时跑 (每路 拉流/解码/推理 3 个线程)
 2. 模型级并行:   同一帧的多个模型同时跑 (线程池, 只在多模型时)
 3. 硬件级并行:   实例绑在 3 个不同的 NPU 核上, 真正同时计算

 限制: NPU 只有 3 个核 -> 同一时刻最多 3 个推理在算, 其余排队
       多模型能降低单帧延迟, 但 NPU 总工作量翻倍
```

### 2.6 实时性：目标帧率控制与丢帧策略

实时视频的原则是**宁可丢帧，不能延迟**。每一级都有明确的丢法，并且丢了多少都会计数：

| 位置 | 丢法 | 计数项 |
|---|---|---|
| 拉流 | 实时流队列满了，丢掉整组画面，等下一个关键帧 | `drop.pkt` |
| 解码后 | 推理没来得及取，新帧覆盖旧帧 | `drop.frame` |
| 推理前 | 超过目标帧率的帧直接丢 | `drop.fps` |
| 推理 | 每 N 帧推理一次，中间帧复用上次的框 | `reuse` |

目标帧率控制有两种用法：

```
 accept(): 丢帧式 (推理线程用)          输入 30fps, target_fps=20
   arrive:  |   |   |   |   |   |   |   |   |      每 33ms 来一帧, 目标周期 50ms
   accept:  Y   .   Y   Y   .   Y   Y   .   Y      每 3 帧留 2 帧 = 20fps; '.' 计入 drop.fps

 wait(): 节拍式 (拼接/推流线程用)
   tick:    |----40ms----|----40ms----|----40ms----|   按固定帧率产出

 两者都按"绝对时间点 += 周期"推进(误差不累积), 落后太多就重新对齐(不追帧)
```

### 2.7 线程安全退出

`std::thread` 有两个坑：
1. 线程对象析构时如果还没 `join`，程序直接 `std::terminate`；
2. 线程里抛出没被捕获的异常，也会 `terminate`。

项目的做法：`WorkerThread` 封装（析构时 join、线程入口统一捕获异常）；退出信号处理函数里只设置一个原子标志；忽略 `SIGPIPE`（推流时对方断开，继续写会触发它，默认直接杀死进程）。退出顺序见第 10 章。

### 2.8 性能统计：让瓶颈自己"说话"

```
 热路径:  ScopedTimer t(stat);  ...  // 析构时 stat->add(ms), 原子累加, 无锁
 perf 线程(每 1s):  exchange 出计数 -> 计算 rate / avg -> 更新 OSD 用的实时值
 每 perf_interval 秒打印:

 stage                 rate/s   avg(ms)   max(ms)    load      total
 ch0.decode              30.0      0.66      1.28      2%        181
 ch0.infer               10.0     13.50     14.18     13%         59   <- load = rate * avg
 yoloA.npu               30.0     12.33     12.51     37%        238
 ch0.drop.frame           0.0         -         -       -          0   <- 纯计数项
 bottleneck(load>85%): ...
```

- **负载** = 每秒次数 × 平均耗时，也就是这一级占用一个线程的比例，接近 100% 说明满负荷。
- **口诀**：看哪一级开始丢帧，瓶颈就在它的下游。

---

## 3. 链路 s1：拉流

代码：`src/stream/StreamLoader.cc`

### 3.1 状态机

```
            start()
               |
               v
        +-------------+  open ok   +-------------+   read error / timeout   +----------------+
        | kConnecting |----------->| kStreaming  |------------------------->| kReconnecting  |
        +-------------+            +-------------+                          +----------------+
               |  open fail               |  EOF && file && !loop                 |  backoff: 500ms,1s,2s...
               v                          v                                       |  (上限 reconnect_max_ms)
        sleep(backoff) --> retry    +-------------+                               |
                                    | kFinished   |  <- 推送 eos 包                +---> openInput() 重试
                                    +-------------+
```

没有画面时，拼接画面里对应的格子会显示 `CONNECTING...` / `RECONNECTING...`。

**重连为什么要逐次翻倍等待**：服务器长时间不可用时不会疯狂重试浪费资源；很多设备同时断线时，也不会在服务器恢复瞬间一起涌上去。

### 3.2 为什么要转 Annex-B

```
 MP4 / FLV 里的 H.264 (AVCC):      [len 4B][NAL][len 4B][NAL]...   SPS/PPS 在 extradata(avcC) 里
 MPP / RTSP 需要的 (Annex-B):      [00 00 00 01][NAL][00 00 01][NAL]...   SPS/PPS 在码流里(关键帧前)

 av_bsf "h264_mp4toannexb": 长度前缀 -> 起始码, 并在关键帧前插入 SPS/PPS
```

### 3.3 超时

网络断了，FFmpeg 的读取可能卡很久。做法是给 FFmpeg 注册一个中断回调，每次读之前设一个截止时间：超时或者程序要退出时，回调让阻塞的读取立即返回，然后走重连流程。

### 3.4 丢包策略：实时流按 GOP 丢

```
 队列满(解码跟不上):
   queue: [P P P P P P ... P]  (64 个)      新来的包 tryPush 失败
   -> clear() 清空积压, drop.pkt += n
   -> wait_keyframe_ = true: 后续 P 帧全部丢弃, 直到下一个 I(IDR) 帧
   -> 从 I 帧重新开始喂解码器 -> 不花屏, 延迟被"截断"不再累积

 为什么不能只丢最老的一个包?  H.264 P 帧依赖前面的帧, 中间丢一个包, 后面整个 GOP 都会花屏
```

文件源则相反：队列满了就等待（背压），文件读得再快也会被解码速度"拖住"，一帧不丢。

### 3.5 文件循环与节奏

- **节奏**：文件按原始帧率的节奏读取，模拟实时流；下游卡住导致落后超过 1 秒时，重新对齐时间基准，不"快进追帧"。
- **循环**：读完回到开头继续读，时间戳接着上一轮往后算，保证下游看到的时间戳一直递增。

---

## 4. 链路 s2：MPP 硬件解码

代码：`src/decode/MppDecoder.cc`

### 4.1 调用流程

```
 init:
   mpp_create(&ctx, &mpi)
   mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC / CodingHEVC)
   mpp_dec_cfg_init -> MPP_DEC_GET_CFG -> set "base:split_parse" -> MPP_DEC_SET_CFG

 decode(pkt):
   mpp_packet_init(&packet, data, size); mpp_packet_set_pts(...)
   loop (最多 250 次, 每次 2ms):
       decode_put_packet(ctx, packet)  ── 成功则 put_done
       drainFrames():
           while decode_get_frame(ctx, &frame) == OK && frame:
               if info_change(frame):                    <- 首帧/分辨率变化
                   mpp_buffer_group_get_internal(DRM)
                   MPP_DEC_SET_EXT_BUF_GROUP
                   mpp_buffer_group_limit_config(buf_size, 24)   <- 最多 24 块, 防止内存失控
                   MPP_DEC_SET_INFO_CHANGE_READY
               else if 正常帧:
                   emit(NV12 {w, h, hor_stride, ver_stride, fd, vaddr})  -> s3 回调
               mpp_frame_deinit(&frame)                   <- 帧 buffer 归还给 group
       if put_done: break;  else usleep(2ms)             <- 输入满: 先取帧腾空间再重试
   mpp_packet_deinit(&packet)

 release:  mpi->reset -> mpp_destroy(ctx) -> mpp_buffer_group_put(grp)   <- 先销毁解码器再释放 buffer
```

### 4.2 要掌握的原理

- **info_change**：第一帧（或分辨率变化时）解码器先通知"需要多大的缓冲区"，按这个大小分配一组缓冲区交给它，并限制数量。
- **缓冲区用完马上归还**：解码器的输出缓冲区是一个池子，不还解码器就卡住；限制数量是为了下游卡住时内存不会无限增长。
- **split_parse**：告诉 MPP 输入是不是完整的一帧。设 1 由 MPP 自己找帧边界（最稳妥，多约一帧延迟），设 0 表示输入已经是整帧（延迟更低）。
- **解码器重建**：每次重连后码流参数可能变（分辨率、编码格式），解码线程发现参数变了就重建解码器；连续大量错误帧时也会重建。
- **兜底**：MPP 不可用或编码格式不支持时，改用 FFmpeg 软件解码。

---

## 5. 链路 s3：NV12 转 BGR

代码：`src/decode/DecodeWorker.cc`（`mpp_decoder_cb`）

### 5.1 NV12 的内存布局（最容易踩的坑）

```
 1920x1080 的帧, MPP 输出 hor_stride=1920, ver_stride=1088 (高按 16 对齐)

 addr 0
 +-------------------- hor_stride (1920) --------------------+
 | Y Y Y Y ... (width=1920 有效)                               |  \
 | ...                                                         |   | height = 1080 行有效
 | ...                                                         |  /
 |- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -|  \ 8 行填充 (1080..1087)
 +-------------------------------------------------------------+  /
 addr = hor_stride * ver_stride   (= 1920*1088, 不是 1920*1080 !)
 | U V U V ... (交错, 每 2x2 像素共享一对 UV)                  |  height/2 = 540 行
 +-------------------------------------------------------------+
```

NV12 格式：先是一整块亮度 Y，后面是颜色 UV 交错存放，每 2×2 个像素共用一组 UV，大小是宽 × 高 × 1.5。人眼对亮度敏感、对颜色不敏感，所以视频都这样压缩颜色。

### 5.2 两条转换路径

```
 RGA 路径(默认):
   MPP DRM buffer --(fd, DMA-BUF)--> RGA imcvtcolor(NV12 -> BGR888) --> cv::Mat(BGR)
   CPU 不碰像素, 零拷贝读取

 CPU 回退路径:
   MPP buffer --逐行 memcpy 去 stride--> 连续 NV12(yuv_) --cv::cvtColor(YUV2BGR_NV12)--> cv::Mat(BGR)

 RGA 返回错误(对齐不满足、>4G 内存等) -> 打一次 warning -> 本路永久改用 CPU 路径
```

**零拷贝**：MPP 解码出的图像在一块硬件内存里，这块内存有一个文件描述符（fd）。把 fd 直接交给 RGA，RGA 用 DMA 直接读这块内存，CPU 不搬数据。

**颜色顺序**：OpenCV 默认是 BGR；RGA 的 `RK_FORMAT_RGB_888` 字节顺序是 R、G、B。搞反了表现为"人脸发蓝"。

---

## 6. 链路 s4：NPU 推理

代码：`src/infer/InferWorker.cc`、`ModelManager.cc`、`rknn_lite.cc`、`postprocess.cc`

### 6.1 推理线程主循环

```
 +--> 从本路的最新帧缓冲取一帧 (序号不连续 -> 中间帧被覆盖, 计入 drop.frame)
 |         |
 |         v
 |    超过目标帧率? --是--> 丢弃 (drop.fps) ----------------------------+
 |         | 否                                                         |
 |         v                                                            |
 |    这帧要推理吗? (每 infer_interval 帧一次, 或上次结果已过期)         |
 |      | 是                                  | 否                      |
 |      v                                     v                         |
 |    所有模型推理 -> 得到 g1..gN          复用上次的融合结果 (reuse)     |
 |    融合 -> 新的检测结果                    |                         |
 |      |                                     |                         |
 |      +------------------+------------------+                         |
 |                         v                                            |
 |    结果没过期就画框 -> 写入本路结果缓冲 images[i] (交给拼接/推流)     |
 |                         |                                            |
 +-------------------------+<-------------------------------------------+
```

跳帧 + 结果复用的时间线（`infer_interval=2`）：

```
 frame:   F1    F2    F3    F4    F5    F6
 infer:   [NPU]       [NPU]       [NPU]
 draw:    R1    R1    R3    R3    R5    R5          <- 显示帧率 = 输入帧率, NPU 只干一半的活
```

### 6.2 模型实例与 NPU 核心绑定

```
 [model0] instances=3, core=auto        [model1] instances=2, core=auto
    inst0: rknn_init  ─┐ 共享权重            inst0: rknn_init  ─┐
    inst1: rknn_dup_context                  inst1: rknn_dup_context
    inst2: rknn_dup_context

 core=auto: 所有模型的实例全局轮询绑定 (rknn_set_core_mask)
    model0.inst0 -> core0   model0.inst1 -> core1   model0.inst2 -> core2
    model1.inst0 -> core0   model1.inst1 -> core1                    <- 三个核负载均衡
 其它取值: any(驱动调度) | 0 | 1 | 2 | 0_1 | 0_1_2(大模型独占三核)
```

- **rknn_dup_context**：新实例共享第一个实例的权重，不重复占用模型内存。
- **独占**：一个实例同一时刻只给一个线程用（见 2.3 的实例池），实例数 = 该模型 NPU 阶段的最大并发度。
- **只在 NPU 阶段占用**：前处理、后处理不占实例（见 2.3），核不会因为 CPU 在做前后处理而空等。

### 6.3 预处理：letterbox 与坐标映射

```
 原图 1280x720  --->  模型输入 640x640
   scale = min(640/1280, 640/720) = 0.5
   new = 640 x 360,  pad_top = (640-360)/2 = 140 (取偶数, 满足 RGA 对齐)

   +------------------------------+
   |  gray 114   (pad_top = 140)  |
   +------------------------------+
   |                              |
   |   resized image 640 x 360    |   <- RGA improcess 一次完成: 缩放 + BGR->RGB + 写入 ROI
   |                              |
   +------------------------------+
   |  gray 114   (140)            |
   +------------------------------+

 模型坐标 -> 原图坐标:
   x_orig = (x_model - pad_left) / scale_x
   y_orig = (y_model - pad_top ) / scale_y      然后裁剪到 [0, 原图宽/高-1]
```

等比缩放加灰边，和 YOLOv5 训练时一致，比直接拉伸准。

### 6.4 YOLOv5 输出解码

```
 3 个输出头 (NCHW, int8):   [1, 255, 80, 80]  stride 8
                            [1, 255, 40, 40]  stride 16
                            [1, 255, 20, 20]  stride 32
 255 = 3 anchors x (5 + 80 classes)    ->  类别数 = C/3 - 5 (代码自动推导)

 通道 c = a*(5+nc) + k,   k: 0=x 1=y 2=w 3=h 4=obj 5..=cls     (模型内已做 sigmoid)
 反量化:  f = (q - zp) * scale
 bx = (x*2 - 0.5 + j) * stride        bw = (w*2)^2 * anchor_w
 by = (y*2 - 0.5 + i) * stride        bh = (h*2)^2 * anchor_h
 score = obj * max(cls)                 score < conf_thresh 丢弃
```

**优化点**：阈值先换算到 int8，直接在量化后的数值上比较，绝大多数格子不需要做浮点计算。

### 6.5 NMS 的正确用法（原代码的 Bug）

NMS（非极大值抑制）：同一个目标往往被框出好几个框，要去重。流程：

```
 按分数从高到低排序
   -> 取出分数最高的框保留
   -> 删掉和它重叠度 (IoU) 超过阈值的"同类"框
   -> 在剩下的框里重复, 直到处理完
```

```
 objProbs:  idx0=0.6(person)  idx1=0.9(car)  idx2=0.8(person)
 按分数排序后 order = [1, 2, 0]      <- order[i] 存的是"原始下标"

 原代码:   if (classIds[i] != filterId)      <- 用排序后的位置 i 去查类别, 下标空间混用!
           内层 if (m == -1 || classIds[i] != filterId)   <- 没检查候选框 m 的类别
 后果:     跨类别误抑制(车把人删了), 同类重复框漏抑制

 正确:     n = order[i]; if (classIds[n] != filterId) continue;
           m = order[j]; if (classIds[m] != filterId) continue;
           if IoU(n, m) > thresh: order[j] = -1       <- 只有高分框抑制同类低分框
```

另一个多线程问题：原代码的类别名是全局数组，加一个"首次加载"的静态标志，多个推理线程第一次同时调用会竞争，每个模型析构时还会释放全局数组。现在改成每个模型自己持有一份，后处理不再有全局可变状态。

---

## 7. 链路 s5：多模型结果融合

代码：`src/fusion/DetectionFusion.cc`

**为什么融合**：单个模型会漏检、误检。多个模型可以：
- **互补**：通用模型找人，专用模型找安全帽；
- **互相核对**：几个模型都认同才算，误报更少；
- **互相兜底**：任一模型看到就保留，漏报更少。

**代价**：NPU 工作量翻倍。**现状**：代码支持多模型，默认只配置了一个模型，这时融合基本等于直接输出。

### 7.1 算法流程

```
 输入: g1 (model0 的框), g2 (model1 的框), ...
  1. 展平所有框, 过滤 score < score_thresh
  2. 按 score * weight 降序排序
  3. 贪心关联: 对每个框, 找 IoU 最大且 > iou_thresh 的"同类簇"
       - 同一个簇里, 每个模型最多贡献一个框 (用 bitmask 记录)
       - 找不到就新建簇
  4. 簇内融合 (method):
       weighted   : WBF 加权框融合
       confidence : 取最高分框, 置信度 noisy-OR
       nms        : 只保留最高分框
  5. 投票: 簇大小(votes) < min_votes 的丢弃
  6. 融合结果再 applyNMS, 去掉相邻簇之间的残留重叠
```

示例（2 个模型）：

```
   model0: A1 person 0.90   A2 car 0.70
   model1: B1 person 0.80 (与 A1 IoU=0.8)   B2 person 0.40 (与 A1 IoU=0.6)

   排序: A1(0.90) B1(0.80) A2(0.70) B2(0.40)
   A1 -> 新簇 C1 {A1}                      mask=01
   B1 -> 与 C1 IoU 0.8 > 0.55, model1 未在 C1 -> 加入 C1 {A1,B1}  mask=11
   A2 -> car, 无同类簇 -> 新簇 C2 {A2}
   B2 -> 与 C1 IoU 0.6, 但 model1 已在 C1 -> 不能加入 -> 新簇 C3 {B2}
          (同一模型的两个框必然是两个目标, 模型内部已经 NMS 过)

   min_votes=1: 输出 C1(2/2), C2(1/2), C3(1/2)
   min_votes=2: 只输出 C1
```

### 7.2 三种合并方式

```
 WBF (weighted):
   box   = Σ(s_i·w_i·box_i) / Σ(s_i·w_i)
   score = [Σ(s_i·w_i) / Σ(w_i)] · min(T, M) / M        T=簇内框数, M=模型总数
          -> 只有部分模型检出的目标, 分数被按比例压低

 confidence (noisy-OR):
   box   = 簇内最高分框
   score = 1 - Π(1 - s_i·w_i)                          -> 多模型一致时分数升高

 nms:
   box, score = 簇内最高分框
```

### 7.3 误检/漏检权衡

| 旋钮 | 调大 | 调小 |
|---|---|---|
| `min_votes` | 交集 → 精度高、漏检多 | 并集 → 召回高、误检多 |
| 模型 `conf_thresh` | 单模型误检少 | 单模型漏检少 |
| 融合 `score_thresh` | 过滤低分框 | 保留更多候选 |
| 模型 `weight` | 该模型话语权大 | 该模型话语权小 |
| `iou_thresh` | 关联更严格(同一目标可能被拆成两个簇) | 关联更宽松(相邻目标可能被合并) |

---

## 8. 链路 s6：拼接与推流

代码：`src/output/Compositor.cc`、`StreamingMgr.cc`

### 8.1 拼接：拉取式节拍

```
 mosaic 线程, 每 1/fps 秒:
   for i in channels: images[i].peek()   (超过 3 秒没新帧 -> 视为无信号)
   combineImage:
   +----------------------+----------------------+
   | CH0 cam0 | 25fps ... | CH1 cam1 | 25fps ... |   <- 每格等比缩放居中 (RGA 或 cv::resize)
   |                      |                      |
   +----------------------+----------------------+
   | CH2 cam2 | 25fps ... | CH3 cam3 |           |
   |                      |   RECONNECTING...    |   <- 没画面时显示 StreamLoader 的状态
   +----------------------+----------------------+
   -> mosaic Mbuffer -> 主线程 imshow / 推流线程
```

### 8.2 推流线程流程

```
 loop @ cfg.fps (FpsController.wait):
   ├─ sink 未打开 && 不需要 SPS/PPS 就能打开(GB28181) && 到了重试时间 -> sink.open()  (启动 SIP)
   ├─ !sink.wantsMedia() -> continue                 GB28181 没人点播时不编码
   ├─ sink 未打开 && 还在退避期 -> continue            断线退避期不编码
   ├─ frame = src.peek()                             src = mosaic 或 images[i]
   ├─ img = resize(frame) 或 frame.clone(); overlayInfo(img)   叠加前必须拷贝(源被共享)
   ├─ 分辨率变化 -> 重建编码器, 并关闭 sink(要按新参数重新建连)
   ├─ sink.takeKeyframeRequest() -> encoder.requestKeyframe()
   ├─ pkts = encoder.encode(img)
   └─ for pkt in pkts:
        ├─ sink 未打开: 非关键帧 -> 请求关键帧并跳过; 关键帧 -> open(带 SPS/PPS)
        │                 open 失败 -> backoff = min(backoff*2, reconnect_max)
        └─ sink.write(pkt) 失败 -> close() -> 退避重连 -> 重连后首帧强制 IDR
```

**为什么要先拷贝再叠加**：源画面被拼接、推流多个线程同时读，直接在上面画会影响别人。

### 8.3 关键帧门控与 SPS/PPS

```
 编码输出:   P  P  P  I(SPS PPS IDR)  P  P ...
 未连接时:   x  x  x  |-> open(extradata=SPS/PPS) -> write(I) -> write(P) ...
             非关键帧直接丢, 并 requestKeyframe(), 让下一帧就是 I 帧

 finalizePacket(): 每个关键帧都检查是否带 SPS(7)/PPS(8) NAL, 没带就把缓存的 header 插到前面
   -> RTSP 客户端中途接入、GB28181 的 PS 流 都依赖带内参数集才能解码
```

- **SPS/PPS**：描述分辨率等解码参数的信息，解码器必须先拿到。
- **MPP 硬件编码**：设置码率、帧率、GOP，并让每个关键帧前都带 SPS/PPS；图像由 RGA 转成 NV12 后直接写进编码器的输入内存；不可用时回退 FFmpeg 软件编码。
- **不用 B 帧**：B 帧要等后面的帧编码完才能输出，会增加延迟。

### 8.4 RTMP 和 RTSP

| | RTMP | RTSP |
|---|---|---|
| 常见用途 | 推给直播服务器 | 监控摄像头 |
| 封装/传输 | FLV 封装，基于 TCP | 视频走 RTP，TCP 或 UDP 可选 |
| 延迟 | 一般 1~3 秒 | 可到几百毫秒 |
| 共同点 | 连接时要 SPS/PPS；连接和写入都有超时；时间戳必须严格递增 | 同左 |

**断线重连**：写失败 → 先正常关闭旧连接（不在服务器上残留旧会话）→ 等待（逐次翻倍）→ 重连 → 第一帧强制关键帧。

**重复会话**：服务器上某个路径已经有人在推流时会拒绝（例如返回 400），程序给出明确提示并稍后重试；配置加载时也会拒绝两个推流项推到同一个地址。

---

## 9. 链路 s6'：GB28181 国标接入

代码：`src/output/Gb28181Sink.cc`

**是什么**：国内视频监控联网的国家标准。公安、政府类项目要求设备都能接入同一个"国标平台"，统一管理、统一看画面。

**和 RTMP/RTSP 的区别**：RTMP/RTSP 是一开机就推流；GB28181 是设备先去平台注册，**等平台点播才发视频**，没人看时不发。

**两部分**：
- **信令**：用 SIP 协议（原本是网络电话协议），负责注册、心跳、点播、挂断；
- **媒体**：H.264 先打包成 PS 格式，再切成 RTP 小包，通过 UDP 或 TCP 发送。

### 9.1 信令时序

```
 Device (Gb28181Sink, thread pushN-sip)                         Platform (e.g. WVP)
   |---- REGISTER (no Authorization) --------------------------------->|
   |<--- 401 Unauthorized (WWW-Authenticate: realm, nonce) ------------|
   |---- REGISTER (Authorization: Digest ... response=MD5(...)) ------>|
   |<--- 200 OK (Expires: 3600) ---------------------------------------|   在 expires*0.8 时刷新注册
   |                                                                   |
   |---- MESSAGE <Notify><CmdType>Keepalive</CmdType> ... ------------>|   每 keepalive 秒一次
   |<--- 200 OK -------------------------------------------------------|   连续 keepalive_max_miss 次没回 -> 重新注册
   |                                                                   |
   |<--- MESSAGE <Query><CmdType>Catalog</CmdType> -------------------|
   |---- 200 OK ------------------------------------------------------>|
   |---- MESSAGE <Response> Catalog (1 channel) --------------------->|
   |                                                                   |
   |<--- INVITE (SDP: c=IP, m=video PORT RTP/AVP, y=SSRC) ------------|   点播
   |---- 100 Trying -------------------------------------------------->|
   |---- 200 OK (SDP: sendonly, rtpmap:96 PS/90000, y=SSRC) --------->|   缓存该 200 OK
   |<--- ACK ----------------------------------------------------------|
   |==== RTP (PT=96, payload = PS) over UDP / TCP ====================>|   ACK 后才开始编码发送
   |                                                                   |
   |<--- BYE ----------------------------------------------------------|
   |---- 200 OK ------------------------------------------------------>|   停止发送, 不再编码
```

认证（Digest）的计算方式——密码本身不在网络上传输：

```
 HA1 = MD5(device_id : realm : password)
 HA2 = MD5("REGISTER" : uri)
 无 qop:        response = MD5(HA1 : nonce : HA2)
 qop=auth:      response = MD5(HA1 : nonce : nc : cnonce : "auth" : HA2)
```

### 9.2 媒体传输三种模式

| 平台 SDP | 含义 | 设备行为 |
|---|---|---|
| `m=video 30000 RTP/AVP` | UDP | 设备 UDP 发到平台 IP:30000 |
| `TCP/RTP/AVP` + `a=setup:passive` | 平台监听 | 设备 ACK 后**主动连接**平台（非阻塞 connect + 3s 超时） |
| `TCP/RTP/AVP` + `a=setup:active` | 平台主动连 | 设备监听端口并写进应答 SDP，ACK 后等平台连入（10s 超时） |

### 9.3 PS 封装结构（了解大致结构即可）

```
 一帧 H.264 (Annex-B) --> PS:

 +--------------+----------------+--------------+-----------------------------+----------------+
 | Pack header  | System header  | PSM          | PES 0xE0 + PTS              | PES 0xE0 ...   |
 | 00 00 01 BA  | 00 00 01 BB    | 00 00 01 BC  | 00 00 01 E0 len 84 80 05 PTS| (无 PTS)        |
 | 14 bytes     | 15 bytes       | 20 bytes     | 14 + <=65000 bytes          | 9 + <=65000    |
 +--------------+----------------+--------------+-----------------------------+----------------+
   每帧都有        仅关键帧          仅关键帧        第一个 PES 带 PTS(90kHz)       帧大于 65000 字节时拆分
                                   含 CRC32         PES_packet_length 最大 65535
```

### 9.4 RTP 分包（了解即可）

```
  0                   1                   2                   3
  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |V=2|P|X|  CC   |M|    PT=96    |        sequence number        |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                     timestamp = pts_ms * 90                   |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                     SSRC = SDP 中 y= 的值                      |
 +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 |                  payload: PS 分片 (<= 1400 B)                  |
 +---------------------------------------------------------------+
   一帧的最后一个分片 M=1; 1400 字节是为了避免 IP 分片

 TCP 模式 (RFC 4571): 每个 RTP 包前加 2 字节长度
   [len_hi len_lo][RTP header + payload][len_hi len_lo][RTP ...]...
```

**SSRC**：平台点播时给的流编号，设备发的每个 RTP 包都要带上，平台靠它认出是哪一路流。填错了平台收到也不认。

### 9.5 端口占用、掉线与重复会话

```
 端口占用:
   SIP: bind(5060) 失败 EADDRINUSE -> 试 5061 ... 最多 20 个, Via/Contact 用实际端口
        注意: UDP 不设 SO_REUSEADDR, 否则两个进程能绑同一端口, 冲突会"静默"发生
   媒体: 配置的 media_port 被占用 -> 回退系统分配端口, 写进应答 SDP

 掉线:
   REGISTER 5s 无应答 -> 5s 后重试
   心跳连续 N 次无应答 -> 判定平台掉线 -> 结束会话 -> 新 Call-ID 重新注册(平台重启后能重新认识设备)

 重复会话:
   同 Call-ID 的 INVITE(平台没收到 200 重传) -> 原样重发缓存的 200 OK, 不建第二路媒体
   推流中收到不同 Call-ID 的 INVITE:
       on_duplicate_invite=replace -> 给旧会话发 BYE, 接受新会话
       on_duplicate_invite=reject  -> 回 486 Busy Here
```

### 9.6 两个线程如何协作

```
 SIP 线程: 收发所有信令, 创建/销毁 session_         (只有它会发 SIP 报文)
 推流线程: write(pkt) -> 持 media_mtx_ -> PS 封装 -> RTP 发送

 推流线程发现 TCP 断开 -> 置 media_failed_ = true (原子变量)
 SIP 线程下一轮 timers() 看到 -> closeSession(true) 发 BYE
   => 所有 SIP 报文都在同一个线程发, 不用给 SIP socket 加锁
```

### 9.7 面试要掌握到什么程度

- **普通 Linux 应用岗**：能说清 GB28181 是什么、和 RTMP/RTSP 的区别，能画出注册和点播流程（9.1），知道心跳和 SSRC 的作用。
- **岗位写了安防、国标**：再了解 Digest 认证怎么算、PS 的大致结构、TCP 模式下谁主动连接、重复点播怎么处理。
- **不用背**：PS 和 RTP 的每一个字段。

---

## 10. 启动与退出顺序

代码：`src/app/App.cc`

```
 init():
   1. 解析配置, 设置日志级别
   2. ModelManager.init()     <- 最先: 最耗时、最容易失败(模型路径/驱动版本/内存)
                                失败直接返回, 此时还没有任何线程和网络连接, 退出干净
   3. DetectionFusion
   4. 创建所有 BlockingQueue / Mbuffer
   5. 创建各级工作对象(还没启动线程)
   6. 启动线程: Perf -> Streaming -> Compositor -> Infer -> Decode -> Loader
                         |<-------- 消费者先就绪 -------->|     生产者最后

 stop():
   1. 通知: 所有 requestStop() + 所有队列/Mbuffer close()   <- 唤醒一切阻塞中的线程
   2. join: Loader -> Decode -> Infer -> Compositor -> Streaming(内部发 trailer/TEARDOWN/BYE/注销)
   3. ModelManager.shutdown(): 先停线程池, 再销毁 dup 实例, 最后销毁 master 上下文
   4. PerfMonitor.stop()

 main():
   signal(SIGPIPE, SIG_IGN)            <- 否则推流对端断开时, 写 socket 会让进程直接退出
   SIGINT / SIGTERM -> g_quit = true   <- 优雅退出
   std::set_terminate(打日志 + abort)  <- 万一 terminate, 至少留下线索和 core
```

---

## 11. 项目难点与应对

每条按"难在哪 → 怎么做 → 一句话总结"组织，方便复习和讲解。

### 难点 1：多路多阶段的解耦与缓冲策略
- **难在哪**：拉流、解码、推理、拼接、推流速度各不相同，任何一级变慢都可能拖垮全链路或造成延迟累积、内存暴涨。
- **怎么做**：压缩码流用有界队列（实时流满了按 GOP 丢、文件流背压）；图像用单槽最新帧 Mbuffer（下游慢自动丢旧帧）；拼接/推流按自己的节拍"拉取"。
- **一句话**：**码流不能随便丢，图像只要最新的。**

### 难点 2：线程安全退出，避免 std::terminate 和卡死
- **难在哪**：十几个线程，阻塞点分布在条件变量、FFmpeg 网络 IO、socket、NPU 调用里；任何一个线程醒不过来，进程就退不出；任何一个 joinable 的 std::thread 被析构，进程就 terminate。
- **怎么做**：WorkerThread 析构 join + 异常捕获；所有等待带超时或可被 close 唤醒；FFmpeg 用 interrupt_callback；先通知全部再按序 join；最后销毁 NPU。
- **一句话**：**先通知、再唤醒、再 join、最后释放共享资源。**

### 难点 3：MPP 的 stride、info_change 与 buffer 管理
- **难在哪**：硬件要求对齐，输出帧的 stride 与宽高不同；首帧才知道 buffer 大小；buffer 由解码器池化管理，不及时归还解码器就会卡住。
- **怎么做**：info_change 时按 buf_size 建 DRM buffer group 并限制数量；回调内同步完成转换后立刻 `mpp_frame_deinit` 归还；CPU 路径严格按 stride 拷贝。
- **一句话**：**硬件 buffer 要"借了马上还"，地址计算一律用 stride。**

### 难点 4：RGA 的对齐/内存限制与自动回退
- **难在哪**：RGA 对宽度对齐、奇数坐标、4G 以上物理内存（RGA2 用虚拟地址时）有限制，报错信息不直观，且不同芯片/驱动版本表现不一。
- **怎么做**：能用 fd 就用 fd（DMA-BUF 零拷贝）；ROI 和尺寸取偶数；任何 RGA 调用失败都回退 OpenCV，并只告警一次，避免日志刷屏。
- **一句话**：**硬件加速是"优化"而不是"依赖"，必须有软件兜底。**

### 难点 5：NPU 多核调度与上下文独占
- **难在哪**：RK3588 NPU 有 3 个核；rknn_context 不能多线程同时用；多模型、多路同时推理时既要吃满三个核，又不能冲突。
- **怎么做**：dup_context 复用权重；实例轮询绑定 core0/1/2；Lease 保证独占；线程池大小 = 实例数，杜绝死锁；多模型并发提交 + future 等待。
- **一句话**：**实例数决定并发度，绑核决定负载均衡。**

### 难点 6：实时性——延迟不能累积
- **难在哪**：实时视频"宁可丢帧，不能延迟"；但丢帧方式不对会花屏，丢得太多又影响效果。
- **怎么做**：拉流级按 GOP 丢；解码后只留最新帧；推理级目标帧率 + 跳帧推理 + 结果复用（带时效）；输出级固定节拍；FpsController 用绝对时间点防漂移、落后就重新对齐不追帧。
- **一句话**：**每一级都要有明确的丢弃策略，并且丢弃要被计数。**

### 难点 7：多模型结果的正确关联
- **难在哪**：不同模型的框位置、分数分布、类别体系都可能不同；简单地把所有框合起来做 NMS，会丢失"多个模型都认同"这一信息。
- **怎么做**：按分数×权重排序后贪心关联；同一簇里每个模型最多一个框；簇内 WBF/noisy-OR；min_votes 控制精度/召回；最后再做一次 NMS。
- **一句话**：**先关联、再融合、再投票。**

### 难点 8：推流的健壮性
- **难在哪**：网络抖动、服务器重启、路径被占用、中途接入的观众看不到画面、时间戳不单调导致 muxer 报错。
- **怎么做**：连接/写入都带超时；断线先关旧连接再指数退避重连；等关键帧再建连；每个关键帧带内携带 SPS/PPS；时间戳强制严格递增；忽略 SIGPIPE。
- **一句话**：**断线是常态，重连要"干净"，首帧必须能解码。**

### 难点 9：GB28181 的信令状态机与互通
- **难在哪**：SIP 事务、Digest 认证、注册刷新、心跳判活、INVITE 重传、重复点播、UDP/TCP 三种媒体模式、PS 位域封装，任何一个细节不对，平台就"注册上了但看不到画面"。
- **怎么做**：独立 SIP 线程统一收发信令；缓存 200 OK 应对重传；replace/reject 策略处理重复点播；PS/RTP 严格按标准位域；用本地模拟平台 + ffprobe 验证 PS 可解码。
- **一句话**：**信令单线程化，媒体按标准逐字节对齐，用工具验证而不是靠猜。**

### 难点 10：性能瓶颈定位
- **难在哪**：流水线里任何一级慢，表现都是"卡"；只看总帧率看不出是谁的问题。
- **怎么做**：每级打点（频率/平均/最大/负载），每级丢帧单独计数；线程命名配合 `top -H`；定频减少测试误差。
- **一句话**：**看哪一级开始丢帧，就知道瓶颈在它的下游。**


---

## 12. 调试方法

GDB、strace、Valgrind 的上手例子见 [DEBUG_TOOLS.md](DEBUG_TOOLS.md)。

### 12.1 工具总览

| 问题类型 | 首选工具 | 备注 |
|---|---|---|
| 逻辑/流程 | 日志 `log_level=debug`、线程名 | SIP 报文在 debug 级别全部打印 |
| 性能瓶颈 | 内置 perf 报告、`top -H`、`perf top`、NPU load | 先用 `performance.sh` 定频 |
| 内存泄漏(普通堆) | ASan/LSan、heaptrack、valgrind massif | PC 上跑最方便 |
| 内存泄漏(硬件内存) | `/proc/meminfo`(CMA)、`dma_buf/bufinfo` | 堆工具**看不到** MPP/RGA/NPU 内存 |
| 句柄泄漏 | `ls /proc/<pid>/fd \| wc -l`、`lsof -p` | 重连场景重点看 socket |
| 数据竞争 | TSan、helgrind | 注意第三方库的误报 |
| 死锁/卡死 | `gdb -p` + `thread apply all bt` | 看谁卡在哪个条件变量上 |
| 崩溃 | core dump + gdb、ASan | 用 RelWithDebInfo 编译保留符号 |
| 流媒体 | ffprobe/ffplay、mediamtx、tcpdump/Wireshark | GB28181 用 Wireshark 的 SIP/RTP 分析 |

### 12.2 性能瓶颈定位

先用 `performance.sh` 把 CPU/NPU 定频，减少测试误差。再对照性能报告：

```
 现象                                   结论
 ------------------------------------  --------------------------------------------
 chN.drop.pkt 持续增长                  解码跟不上 -> 看 chN.decode / chN.yuv2bgr 负载
 chN.drop.frame 持续增长                推理跟不上 -> 看 chN.infer / <model>.npu / <model>.wait
 <model>.wait 平均耗时大                实例不够, 大家在排队抢实例 -> 增加 instances
 <model>.pre 很大                       RGA 回退到了 CPU(日志找 "rga ... fallback"),
                                        或 RGA 用普通内存反而慢 -> [infer] use_rga=0 对比
 chN.wait_frame 很大                    推理比出帧快, 在等新帧(正常); 或解码出帧不均匀
 NPU 核占用低但每路帧率上不去          每路一次只处理一帧, 单帧耗时太长 -> 先压前处理
 pushN.encode 很大                      MPP 编码没启用(回退 libx264)? 看启动日志的编码器名
 mosaic.compose 很大                    拼接用了 CPU 缩放, 或拼接分辨率过高
 某阶段 load 接近 100%                  该线程已饱和, 它就是瓶颈
```

硬件占用：`top -H` 看各线程 CPU，`/sys/kernel/debug/rknpu/load` 看 3 个 NPU 核各自的占用（需要 root）。

### 12.3 内存泄漏排查

**第一步：判断是不是真的泄漏**

```
 RSS 持续增长?
   |
   +-- 否, 涨到一定程度后稳定
   |     -> 不是泄漏: 分配器缓存 / 内存碎片 / 各级缓冲填满
   |        (glibc 每线程 arena 会缓存内存: 可试 MALLOC_ARENA_MAX=2 或定期 malloc_trim(0) 验证)
   |
   +-- 是, 随运行时间线性增长
         |
         +-- /proc/<pid>/fd 数量也在涨?     -> 句柄泄漏 (socket / avformat / 文件未关闭)
         +-- Threads 数量在涨?              -> 线程泄漏 (线程未 join / 不断新建)
         +-- CMA / dma_buf 在涨, 堆没涨?     -> 硬件内存泄漏 (MPP/RGA/RKNN buffer 未释放)
         +-- 普通堆在涨                      -> ASan/LSan / heaptrack / massif 找分配点
```

看趋势不看绝对值，要跑够时间（覆盖几次重连、几轮文件循环）。启动后几分钟内上涨是正常的（缓冲区填满、线程池建立）。

**第二步：用工具定位**
- **ASan**：编译时加 `-fsanitize=address`，程序**正常退出**（Ctrl+C，不能 kill -9）时打印泄漏的分配位置，还能查越界、释放后使用；
- **heaptrack**：开销小，适合长时间跑，能看哪一行分配的内存最后没释放；
- **valgrind**：不用重新编译，但慢几十倍，适合在 PC 上短时间跑。

**第三步：硬件内存单独看**

MPP 的解码/编码缓冲区、NPU 的内存都不走 `malloc`，ASan、valgrind **看不到**。只能：
- 看系统的 CMA 可用量（`/proc/meminfo` 里的 CmaFree）是否持续下降；
- 检查每个"申请"都有对应的"释放"：

| 资源 | 申请 | 释放 | 漏了会怎样 |
|---|---|---|---|
| 解码出的帧 | `decode_get_frame` | `mpp_frame_deinit` | 缓冲区池被占满，解码器卡死 |
| 解码缓冲区组 | 创建 buffer group | 先销毁解码器，再释放 group | 硬件内存泄漏 |
| NPU 推理输出 | `rknn_outputs_get` | `rknn_outputs_release` | 每帧泄漏约 2MB |
| NPU 上下文 | `rknn_init` / `rknn_dup_context` | `rknn_destroy`（先 dup 实例，后第一个实例） | 退出时泄漏/崩溃 |
| FFmpeg 输入/输出 | 打开输入/输出 | 关闭输入/输出 | 重连时连接和内存越积越多 |
| socket | `socket()` | `close()`（重连、会话结束、析构都要关） | 打开的文件数一直涨 |
| 线程 | 启动线程 | 析构时 `join` | 线程数一直涨 / terminate |

**"逻辑泄漏"**：指针没丢，但内存一直涨。常见原因：
- 队列不设上限；
- 某个线程一直拿着一帧大图不放；
- 下游连不上时还在不停编码缓存。

本项目所有队列有界、最新帧缓冲只存 1 帧、重连等待期间和国标没人点播时都不编码。

**第四步：用配置二分缩小范围**

```
 1. [pushN] enable=0            -> 泄漏消失?  问题在推流/编码
 2. display=0                   -> 泄漏消失?  问题在 GUI (highgui 后端)
 3. use_rga=0                   -> 泄漏消失?  问题在 RGA 路径
 4. [decoder] backend=ffmpeg    -> 泄漏消失?  问题在 MPP 路径
 5. 只保留 1 路 source          -> 泄漏速度减为 1/4?  每路都在泄漏(按路分配的资源)
 6. 输入换成本地文件            -> 泄漏消失?  问题在网络重连路径
```

### 12.4 数据竞争

用 ThreadSanitizer（编译加 `-fsanitize=thread`）。注意误报：FFmpeg、OpenCV 内部用原子操作管理引用计数，这些库没有被插桩，TSan 看不到同步关系，会把"最后一个持有者释放内存"报成竞争。判断方法：看报告里冲突的两边是不是都在第三方库的引用计数/释放逻辑里、访问的是不是我们自己的变量。本项目实测的 5 条报告都属于这种误报。

### 12.5 卡死

`gdb -p <进程号>`，输入 `thread apply all bt` 看每个线程卡在哪：
- 卡在条件变量上：它在等谁、谁应该叫醒它？
- 卡在锁上：检查两个线程的加锁顺序；
- 卡在网络调用上：有没有超时？
- 退出时卡住：最常见的是某个缓冲区没关闭，线程还在等。

### 12.6 崩溃

打开 core dump（`ulimit -c unlimited`），崩溃后用 `gdb 程序 core文件` 看调用栈。常见崩溃原因：

| 栈顶位置 | 可能原因 |
|---|---|
| `std::terminate` / `abort` | joinable 的 std::thread 被析构、线程里有未捕获异常（本项目 `set_terminate` 会先打日志） |
| `rknn_run` / `rknn_outputs_get` | 上下文已销毁还在用（退出顺序错）、多线程同时用一个上下文 |
| `mpp_*` | buffer group 已 put 但解码器还在用（释放顺序错） |
| `cv::Mat` 析构 / `free` | 重复释放、越界写坏了堆 → 用 ASan 复现 |
| `improcess` / `imcvtcolor` | 传入的 buffer 尺寸与实际分配不符 |
| 进程无栈直接消失 | 被 SIGPIPE 杀死（检查是否忽略了 SIGPIPE）、被 OOM killer 杀死（`dmesg \| grep -i oom`） |

### 12.7 流媒体问题

- **本地测试**：用 mediamtx（单文件的 RTSP/RTMP 服务器）在本机起一个服务器；
- **看推出去的流对不对**：`ffprobe` 看编码和分辨率，`ffplay` 播放；
- **国标问题**：用 tcpdump 抓包，再用 Wireshark 过滤 `sip` 看信令时序；
- **验证 PS 流**：把收到的 RTP 包去掉包头、拼起来，用 `ffprobe -f mpeg` 看能不能识别出 H.264。

### 12.8 常见问题速查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 画面发绿/颜色错位 | NV12 的 UV 起点按 `width*height` 算了 | 用 `hor_stride*ver_stride`，按 stride 逐行拷贝 |
| 人脸发蓝/颜色互换 | RGB/BGR 搞反 | OpenCV 默认 BGR；RGA 的 `RK_FORMAT_RGB_888` 字节序是 R,G,B |
| 框整体偏移/偏大 | letterbox 的 pad/scale 没有反算 | `x = (x_model - pad) / scale`，再裁剪到原图 |
| 同类框重叠没去掉 / 不同类互相抑制 | NMS 下标混用 | 见 6.5 |
| 延迟越来越大 | 实时流用了无界或阻塞队列 | 实时流用 tryPush + GOP 丢弃；图像用最新帧槽 |
| 解码花屏一段时间 | 队列满后随机丢了 P 帧 | 丢到下一个关键帧为止 |
| 推流播放端黑屏/等很久才出画面 | 首帧不是关键帧，或关键帧没带 SPS/PPS | 关键帧门控 + 带内 SPS/PPS + 新会话强制 IDR |
| 推流断开后进程直接没了 | SIGPIPE | `signal(SIGPIPE, SIG_IGN)` |
| RTSP 推流 400 | 该路径已被别人推 | 换路径或让对方停止；检查自己是否残留旧会话 |
| GB28181 注册上了但没画面 | SSRC 不对、PT 不是 96、PS 格式错、TCP 主被动搞反 | 抓包对照 SDP 的 y=、setup；用 ffprobe 验证 PS |
| GB28181 反复重新注册 | 心跳没回（防火墙/NAT）、密码错（401 循环） | 日志里看 "keepalive no response" 或 "rejected (401)" |
| 退出时卡住 | 某个队列/Mbuffer 没 close、网络调用无超时 | `gdb thread apply all bt` 看卡在哪 |
| 退出时 terminate | joinable 的 std::thread 被析构 | 用 WorkerThread，析构中 join |
| NPU 利用率低 | 实例数少、全绑在一个核 | 增加 instances，core=auto |
| RGA 报错然后变慢 | 对齐不满足/内存 >4G，已回退 CPU | 看日志 "rga ... fallback"；尺寸取 16 对齐 |
| `Too many open files` | 重连时 socket/avformat 没关 | `ls /proc/<pid>/fd` 看增长的是什么 |

---

## 13. 学习路线与练习

### 13.1 建议学习顺序

```
 第 1 天: 全局        看懂 1.2 全链路图和 1.3 线程地图, 能自己画出来
 第 2 天: 核心原理    两种缓冲区 / 实例池 / 线程池 / 丢帧策略 / 线程安全退出
 第 3 天: 输入        拉流(超时、重连、GOP 丢包) + MPP 解码(流程、stride)
 第 4 天: 推理        实例与绑核 / letterbox 坐标映射 / NMS
 第 5 天: 融合+输出   融合流程 + 拼接 + 推流(关键帧门控、断线重连)
 第 6 天: 国标        注册和点播流程, 能在纸上画出 9.1 的时序图
 第 7 天: 调试        性能报告怎么看, 内存泄漏排查流程, 卡死/崩溃怎么查
```

### 13.2 练习题（由易到难）

1. **加一个 OSD 字段**：在每路画面上显示当前检测到的目标数量。
2. **修复一个已知小问题**：实时流重连后，`StreamLoader` 会把 `first_pts_` 重置但没有更新 `pts_offset_`，导致该路帧的 pts 从 0 重新开始（推流用自己的时钟，所以不影响推流）。请让它在重连后继续单调递增。
3. **perf 报告加内存行**：每次打印时附带 VmRSS、线程数、fd 数，方便长时间观察泄漏。
4. **故意制造泄漏**：注释掉 `rknn_outputs_release`，用 CMA/RSS 曲线观察现象；再在 FFmpeg 路径注释掉 `av_packet_unref`，用 ASan 找出来。
5. **GB28181 支持 H.265**：PSM 的 stream_type 改为 0x24，编码器改为 HEVC，注意 finalizePacket 的 NAL 类型判断也要改。
6. **零拷贝 NPU 输入**：用 `rknn_create_mem` + `rknn_set_io_mem`，让 RGA 直接把预处理结果写进 NPU 输入内存，省掉一次拷贝。
7. **更好的关联算法**：把贪心关联改成匈牙利匹配，对比拥挤场景下的效果。
8. **SIP over TCP**：给 GB28181 增加 TCP 信令通道。
