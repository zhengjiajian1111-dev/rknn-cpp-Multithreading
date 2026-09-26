# rknn-cpp-Multithreading 项目学习指南

> 面向：想吃透本项目，或准备用它做面试/项目讲解的同学。
> 读法：第 1 章建立全局印象 → 第 2 章先学"地基"(队列、线程、帧率控制) → 第 3~9 章沿数据流逐段深入 → 第 10 章看启动/退出 → 第 11 章项目难点 → 第 12 章调试方法(内存泄漏、死锁、崩溃、性能、流媒体)。
> 约定：图中方框内只用英文(保证等宽对齐)，中文注释写在框外。文中 `文件:行号` 以当前 main 分支为准，代码演进后行号可能略有偏移，以函数名为准。

---

## 目录

- [1. 全局：一条完整链路](#1-全局一条完整链路)
- [2. 地基：线程、队列与帧率控制](#2-地基线程队列与帧率控制)
- [3. 链路 s1：拉流 StreamLoader](#3-链路-s1拉流-streamloader)
- [4. 链路 s2：MPP 硬件解码](#4-链路-s2mpp-硬件解码)
- [5. 链路 s3：NV12 转 BGR（mpp_decoder_cb + RGA）](#5-链路-s3nv12-转-bgrmpp_decoder_cb--rga)
- [6. 链路 s4：RKNN 推理](#6-链路-s4rknn-推理)
- [7. 链路 s5：多模型结果融合](#7-链路-s5多模型结果融合)
- [8. 链路 s6：拼接、编码与 RTMP/RTSP 推流](#8-链路-s6拼接编码与-rtmprtsp-推流)
- [9. 链路 s6'：GB28181 国标接入](#9-链路-s6gb28181-国标接入)
- [10. 生命周期：初始化与退出顺序](#10-生命周期初始化与退出顺序)
- [11. 项目难点与应对](#11-项目难点与应对)
- [12. 调试方法大全](#12-调试方法大全)
- [13. 练习题与学习路线](#13-练习题与学习路线)
- [附录 A：配置速查](#附录-a配置速查)
- [附录 B：关键 API 速查](#附录-b关键-api-速查)
- [附录 C：PC 无板调试用的 RKNN 桩库](#附录-cpc-无板调试用的-rknn-桩库)

---

## 1. 全局：一条完整链路

### 1.1 一句话

在 RK3588 上同时接入 4 路视频，用 **MPP 硬解 → RGA 转换 → RKNN 多模型并行推理 → 多模型结果融合**，把结果**拼接显示**，并按配置同时推到 **RTMP / RTSP / GB28181** 平台。

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

排查问题时用 `top -H -p $(pidof rknn_multi_stream)` 可以直接看到上面这些线程名和各自的 CPU 占用（线程名由 `setThreadName` → `pthread_setname_np` 设置）。

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

### 1.6 技术栈一览

| 层 | 技术 | 本项目用途 | 缺失时 |
|---|---|---|---|
| 语言/构建 | C++14, CMake, pkg-config | 全部 | — |
| 并发 | std::thread / mutex / condition_variable / atomic, dpool 线程池 | 队列、最新帧槽、推理池 | — |
| 解封装/封装 | FFmpeg libavformat + bsf | 拉流、Annex-B 转换、FLV/RTSP 推流 | 必需 |
| 编解码 | Rockchip **MPP** | H.264/H.265 硬解, H.264 硬编 | 回退 FFmpeg 软编解码 |
| 2D 加速 | **librga** (im2d 1.9.x) | NV12↔BGR、缩放、letterbox、拼接缩放 | 回退 OpenCV |
| NPU | **RKNN Runtime** 1.5.2 | 多实例、共享权重、绑核推理 | 必需 |
| 图像 | OpenCV 4 | 画框、OSD、显示、CPU 回退 | 必需 |
| 国标 | 自研 SIP / PS / RTP / MD5 | GB28181 设备端 | 无外部依赖 |

---

## 2. 地基：线程、队列与帧率控制

后面所有模块都建立在这几个小组件上，**先把它们读懂，再读业务代码会轻松很多**。

### 2.1 两种缓冲区：BlockingQueue vs Mbuffer

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

### 2.2 条件变量的正确写法

```cpp
// 1. 永远带谓词, 防虚假唤醒;  2. 带超时或能被 close() 唤醒, 防退出卡死
std::unique_lock<std::mutex> lock(mtx_);
if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                  [&] { return closed_ || data_.seq > after_seq; }))
    return false;              // 超时
if (closed_) return false;     // 被关闭
out = data_;                   // 锁内只做浅拷贝(引用计数 +1), 不做重活
```

### 2.3 目标帧率控制 FpsController

```
 accept(): 丢帧式 (推理线程用)          输入 30fps, target_fps=20
   arrive:  |   |   |   |   |   |   |   |   |      每 33ms 来一帧, 目标周期 50ms
   accept:  Y   .   Y   Y   .   Y   Y   .   Y      每 3 帧留 2 帧 = 20fps; '.' 计入 chN.drop.fps
   deadline 按"绝对时间点 += 周期"推进, 允许 20% 抖动, 落后太多就重新对齐(不追帧)

 wait(): 节拍式 (拼接/推流线程用)
   tick:    |----40ms----|----40ms----|----40ms----|   按固定帧率产出
   sleep_until(next); next += period;  处理超时则不睡并重新对齐
```

### 2.4 WorkerThread：为什么不会 std::terminate

`std::thread` 有两个"地雷"：
1. 析构时如果还 `joinable()`，直接 `std::terminate()`；
2. 线程函数里逃逸出的异常，也会 `std::terminate()`。

`WorkerThread`(include/common/WorkerThread.hpp) 的对策：析构函数里 `join()`；线程入口统一 `try/catch` 并打日志；启动时设置线程名。此外所有阻塞点都能被停止标志或 `close()` 打断，所以 join 一定能返回。

### 2.5 PerfMonitor：让瓶颈自己"说话"

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

---

## 3. 链路 s1：拉流 StreamLoader

文件：`src/stream/StreamLoader.cc`

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

`state()` 会被拼接线程读取，没有画面时格子里显示 `CONNECTING...` / `RECONNECTING...`。

### 3.2 为什么必须转 Annex-B

```
 MP4 / FLV 里的 H.264 (AVCC):      [len 4B][NAL][len 4B][NAL]...   SPS/PPS 在 extradata(avcC) 里
 MPP / RTSP 需要的 (Annex-B):      [00 00 00 01][NAL][00 00 01][NAL]...   SPS/PPS 在码流里(关键帧前)

 av_bsf "h264_mp4toannexb": 长度前缀 -> 起始码, 并在关键帧前插入 SPS/PPS
```

对 RTSP 来的本来就是 Annex-B 的流，这个 bsf 会原样放行，所以统一挂上即可。

### 3.3 超时：interrupt_callback

FFmpeg 的 `av_read_frame`、`avformat_open_input` 在网络异常时可能阻塞很久。做法：

```cpp
fmt_->interrupt_callback.callback = &StreamLoader::interruptCallback;   // 返回 1 = 中止阻塞调用
armDeadline(cfg_.timeout_ms);                                          // 每次阻塞调用前设截止时间
int ret = av_read_frame(fmt_, pkt);
// interruptCallback: !running_ (要退出了) 或 now > deadline (超时) -> 返回 1
```

注意 RTSP 的 socket 超时选项名随版本变化：FFmpeg ≥ 5 用 `timeout`，4.x 用 `stimeout`（4.x 的 `timeout` 表示**监听模式**，用错了会变成等别人连进来）。代码里用 `LIBAVFORMAT_VERSION_MAJOR` 区分。

### 3.4 丢包策略：实时流按 GOP 丢

```
 队列满(解码跟不上):
   queue: [P P P P P P ... P]  (64 个)      新来的包 tryPush 失败
   -> clear() 清空积压, drop.pkt += n
   -> wait_keyframe_ = true: 后续 P 帧全部丢弃, 直到下一个 I(IDR) 帧
   -> 从 I 帧重新开始喂解码器 -> 不花屏, 延迟被"截断"不再累积

 为什么不能只丢最老的一个包?  H.264 P 帧依赖前面的帧, 中间丢一个包, 后面整个 GOP 都会花屏
```

文件源则用阻塞 `push()`：文件读得再快也会被解码速度"拖住"（背压），一帧不丢。

### 3.5 文件循环与节奏

- **节奏**：`realtime=1` 时，按 `dts` 与墙上时钟对齐 `sleep`，模拟实时流；若下游阻塞导致落后超过 1 秒，就重置时间基准，避免之后"快进追帧"。
- **循环**：读到 EOF 后 `av_seek_frame` 回开头、`av_bsf_flush`，并把 `pts_offset_` 设为上一轮最后的 pts + 一帧，保证下游 pts 单调递增。

---

## 4. 链路 s2：MPP 硬件解码

文件：`src/decode/MppDecoder.cc`

### 4.1 调用时序

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

### 4.2 split_parse 是什么

- `split_parse=1`：MPP 内部自己找帧边界。输入可以是任意切片的码流（例如直接读 .h264 文件），最稳妥，代价是多约一帧延迟（要看到下一帧开头才知道这一帧结束）。
- `split_parse=0`：调用方保证每次送的是完整一帧（FFmpeg 读出来的就是整帧），延迟更低。
- 配置项 `[decoder] mpp_split_parse`，默认 1。

### 4.3 解码器何时重建

- 每次(重新)打开输入，StreamLoader 都生成新的 `StreamParams` 对象；解码线程发现 `pkt->params` 指针变了，就销毁重建解码器（重连后分辨率/编码格式可能变）。
- MPP 连续 50 帧以上错误帧 → `decode()` 返回 -1 → 下一个包到来时重建。
- `backend=auto` 时 MPP 初始化失败或编码格式不是 H.264/H.265 → 回退 `FfmpegDecoder`。

---

## 5. 链路 s3：NV12 转 BGR（mpp_decoder_cb + RGA）

文件：`src/decode/DecodeWorker.cc`, `src/common/RgaUtils.cc`

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

**典型 Bug**：按 `width*height` 找 UV 起点，会从 Y 的填充区开始读色度 → 画面颜色整体错位/发绿。本项目 CPU 路径逐行拷贝去掉 stride，UV 起点用 `hor_stride * ver_stride`。

### 5.2 两条转换路径

```
 RGA 路径(默认):
   MPP DRM buffer --(fd, DMA-BUF)--> RGA imcvtcolor(NV12 -> BGR888) --> cv::Mat(BGR)
   CPU 不碰像素, 零拷贝读取

 CPU 回退路径:
   MPP buffer --逐行 memcpy 去 stride--> 连续 NV12(yuv_) --cv::cvtColor(YUV2BGR_NV12)--> cv::Mat(BGR)

 RGA 返回错误(对齐不满足、>4G 内存等) -> 打一次 warning -> 本路永久改用 CPU 路径
```

RGA 的格式命名：`RK_FORMAT_RGB_888` 内存字节序是 R,G,B（对应 OpenCV 的 RGB），`RK_FORMAT_BGR_888` 是 B,G,R（对应 OpenCV 默认的 BGR）。搞反了表现为"人脸发蓝"。

---

## 6. 链路 s4：RKNN 推理

文件：`src/infer/InferWorker.cc`, `ModelManager.cc`, `rknn_lite.cc`, `preprocess.cc`, `postprocess.cc`

### 6.1 推理线程主循环

```
 while running:
   frame = decoded[i].waitNew(last_seq)            // 拿最新帧; seq 不连续 -> drop.frame += gap
   if !fps.accept(): drop.fps++; continue          // 目标帧率控制
   do_infer = (processed % infer_interval == 0) || 结果过期(reuse_max_ms)
   if do_infer:
       models.inferAll(frame.img, groups)          // 多模型并发提交 + 等待 -> g1..gN
       dets = fusion.fuseDetections(groups)        // s5
   else:
       reuse++                                     // 跳帧: 复用上次的 dets
   if dets 未过期: drawFusedDetections(frame.img, dets)
   images[i].write(frame.img)                      // 交给拼接/推流
```

跳帧 + 结果复用的时间线（`infer_interval=2`）：

```
 frame:   F1    F2    F3    F4    F5    F6
 infer:   [NPU]       [NPU]       [NPU]
 draw:    R1    R1    R3    R3    R5    R5          <- 显示帧率 = 输入帧率, NPU 只干一半的活
```

### 6.2 模型实例池与 NPU 核心绑定

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

- **rknn_dup_context**：新上下文共享第一个实例的权重，只额外占用少量运行时内存。
- **独占**：一个 rknn_context 同一时刻只能一个线程用。`ModelManager::Lease` 是 RAII 租借：构造时从空闲列表取一个实例（没有就在条件变量上等），析构时归还。实例数 = 该模型的最大并发度。

### 6.3 线程池并发提交与等待

```
 单线程串行:   |--model0 12ms--|--model1 12ms--|              = 24ms
 并发(本项目): |--model0 12ms--|                                = ~13ms
               |--model1 12ms--|
 代码:
   for k in models: futs[k] = pool->submit([&]{ Lease l(slot[k]); return l->interf(img, groups[k]); })
   for f in futs:   f.get()      // 必须全部等完: 任务引用了调用方栈上的 img / groups
```

- 线程池大小 = 所有实例数之和：每个线程最多持有一个实例，持有者不再等待其它资源 → **不会死锁**。
- 只有一个模型时，直接在推理线程里执行，省掉一次线程切换（4 路推理线程本身就提供并发）。

### 6.4 预处理：letterbox 与坐标映射

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

用灰边等比缩放（letterbox）与 YOLOv5 训练时一致，比直接拉伸精度好；`letterbox=0` 则直接拉伸。

### 6.5 YOLOv5 输出解码（int8）

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

**优化点**：阈值先换算到 int8 域（`qnt_f32_to_affine`），在量化域比较 `obj` 和 `cls`，绝大多数格子不需要做浮点反量化。

### 6.6 NMS 的正确用法（原代码的 Bug）

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

另一个多线程 Bug：原 `postprocess.cc` 用全局 `labels[]` + 静态"首次加载"标志，多个推理线程同时首次调用会竞争，而每个模型析构时又会 `free` 全局标签。现在改为每个模型持有 `shared_ptr<const vector<string>>`，`post_process` 不再有任何全局可变状态。

---

## 7. 链路 s5：多模型结果融合

文件：`src/fusion/DetectionFusion.cc`

### 7.1 算法步骤

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

### 7.2 三种簇内融合公式

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

## 8. 链路 s6：拼接、编码与 RTMP/RTSP 推流

文件：`src/output/Compositor.cc`, `StreamingMgr.cc`, `MppEncoder.cc`, `FfmpegEncoder.cc`, `VideoEncoder.cc`, `FfmpegPushSink.cc`

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

**为什么是"拉"不是"推"**：如果每路结果都去"推动"拼接，某一路卡住就会拖慢整个画面；按固定节拍拉取最新帧，卡住的那一路只是画面不更新。

### 8.2 推流线程 streamingWorker

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

### 8.3 关键帧门控与带内 SPS/PPS

```
 编码输出:   P  P  P  I(SPS PPS IDR)  P  P ...
 未连接时:   x  x  x  |-> open(extradata=SPS/PPS) -> write(I) -> write(P) ...
             非关键帧直接丢, 并 requestKeyframe(), 让下一帧就是 I 帧

 finalizePacket(): 每个关键帧都检查是否带 SPS(7)/PPS(8) NAL, 没带就把缓存的 header 插到前面
   -> RTSP 客户端中途接入、GB28181 的 PS 流 都依赖带内参数集才能解码
```

- **MPP 硬编**：`MPP_ENC_SET_CFG` 设置 CBR 码率、帧率、GOP、H.264 High Profile；`MPP_ENC_SET_HEADER_MODE = EACH_IDR` 让每个 IDR 前都输出 SPS/PPS；`MPP_ENC_GET_HDR_SYNC` 取 header 作为 extradata。BGR→NV12 由 RGA 直接写进 MPP 的 DRM buffer。
- **FFmpeg 回退**：依次尝试 `h264_rkmpp, libx264, libopenh264, h264_v4l2m2m, h264`；libx264 用 `ultrafast + zerolatency`，不用 B 帧（dts == pts，延迟低）。

### 8.4 RTMP / RTSP 出口

| | RTMP | RTSP |
|---|---|---|
| FFmpeg 格式 | `flv` + `avio_open2` | `rtsp`（ANNOUNCE/SETUP/RECORD，`rtsp_transport` 可选 tcp/udp） |
| extradata | 必须（FLV 需要 AVC sequence header） | 必须（SDP 的 sprop-parameter-sets） |
| 超时 | `interrupt_callback` + 截止时间（连接/写帧 `timeout_ms`，关闭 2s） | 同左 |
| 时间戳 | 毫秒，保证严格递增（否则 muxer 报错） | 同左，muxer 内部转 90kHz |

**重复会话**：服务器上某路径已有推流者时，RTSP 服务器通常返回 4xx（如 mediamtx 返回 400），代码识别 `Server returned 4` 并提示"路径可能已被占用"，然后退避重试。重连前总是 `av_write_trailer` + 关闭旧连接，避免自己在服务器上残留一个"僵尸会话"。配置加载时也会拒绝两个推流项写同一个 URL。

---

## 9. 链路 s6'：GB28181 国标接入

文件：`src/output/Gb28181Sink.cc`, `src/output/gb28181/*`

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

Digest 认证计算：

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

### 9.3 PS 封装结构

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

- Pack header 里的 SCR、PES 里的 PTS 都是 33 bit、90kHz，按标准位域打包（每段之间有 marker bit）。
- PSM 里 `stream_type = 0x1B` 表示 H.264（H.265 是 0x24，本项目目前只做了 H.264）。

### 9.4 RTP 分包

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

---

## 10. 生命周期：初始化与退出顺序

文件：`src/app/App.cc`

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

**为什么顺序这么重要**：
- 如果先启动拉流、后加载模型，模型加载失败时已经有网络连接和线程在跑，退出要处理一堆半初始化状态。
- 如果先销毁 NPU 上下文再 join 推理线程，推理线程可能正在 `rknn_run` → 段错误。
- 如果 join 前不 `close()` 队列，阻塞在 `pop()` 上的线程永远醒不过来 → 退出卡死。

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

## 12. 调试方法大全

### 12.1 工具总览

| 问题类型 | 首选工具 | 备注 |
|---|---|---|
| 逻辑/流程 | 日志 `log_level=debug`、线程名 | SIP 报文在 debug 级别全部打印 |
| 性能瓶颈 | 内置 perf 报告、`top -H`、`perf top`、NPU load | 先用 `performance.sh` 定频 |
| 内存泄漏(普通堆) | ASan/LSan、heaptrack、valgrind massif | PC 桩库版本最方便 |
| 内存泄漏(硬件内存) | `/proc/meminfo`(CMA)、`dma_buf/bufinfo` | 堆工具**看不到** MPP/RGA/NPU 内存 |
| 句柄泄漏 | `ls /proc/<pid>/fd \| wc -l`、`lsof -p` | 重连场景重点看 socket |
| 数据竞争 | TSan、helgrind | 注意第三方库的误报 |
| 死锁/卡死 | `gdb -p` + `thread apply all bt` | 看谁卡在哪个条件变量上 |
| 崩溃 | core dump + gdb、ASan | 用 RelWithDebInfo 编译保留符号 |
| 流媒体 | ffprobe/ffplay、mediamtx、tcpdump/Wireshark | GB28181 用 Wireshark 的 SIP/RTP 分析 |

### 12.2 日志与线程名

```bash
# config/app.ini 里 [general] log_level = debug  (会打印每条 SIP 收发报文)
./rknn_multi_stream -c config/app.ini 2>&1 | tee run.log

# 看每个线程的 CPU 占用(线程名: ch0-demux / ch0-dec / ch0-infer / mosaic / push0 / push0-sip / perf)
top -H -p $(pidof rknn_multi_stream)
```

日志格式：`时:分:秒.毫秒 [级别] [线程名] 内容`，警告和错误输出到 stderr。

### 12.3 性能瓶颈定位

**第一步：定频**，减少测试误差（root 运行 `performance.sh`：CPU 各簇 userspace 定频、NPU 定频 1GHz）。

**第二步：读内置 perf 报告**：

```
 现象                                   结论
 ------------------------------------  --------------------------------------------
 chN.drop.pkt 持续增长                  解码跟不上 -> 看 chN.decode / chN.yuv2bgr 负载
 chN.drop.frame 持续增长                推理跟不上 -> 看 chN.infer / <model>.npu / <model>.wait
 <model>.wait 平均耗时大                实例不够, 大家在排队抢实例 -> 增加 instances
 <model>.pre 很大                       RGA 失败回退到了 CPU? 日志里找 "rga ... fallback"
 pushN.encode 很大                      MPP 编码没启用(回退 libx264)? 看启动日志的编码器名
 mosaic.compose 很大                    拼接用了 CPU 缩放, 或拼接分辨率过高
 某阶段 load 接近 100%                  该线程已饱和, 它就是瓶颈
```

**第三步：看硬件负载**：

```bash
sudo cat /sys/kernel/debug/rknpu/load            # NPU 三个核各自的占用 (需 root + debugfs)
cat /sys/class/devfreq/fdab0000.npu/cur_freq     # NPU 当前频率
sudo perf top -p $(pidof rknn_multi_stream)      # CPU 热点函数
```

**第四步：看模型逐层耗时**（需要时临时改代码）：`rknn_init` 时带 `RKNN_FLAG_COLLECT_PERF_MASK`，`rknn_run` 后用 `rknn_query(ctx, RKNN_QUERY_PERF_DETAIL, ...)` 取逐层耗时，`RKNN_QUERY_PERF_RUN` 取单次运行时间；`RKNN_QUERY_MEM_SIZE` 可查权重/内部内存大小。环境变量 `RKNN_LOG_LEVEL` 可以提高 runtime 的日志详细程度。

### 12.4 内存泄漏调试（重点）

#### 12.4.1 先判断：是不是真的泄漏

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

**观察命令**（建议开两个终端，一边跑一边看）：

```bash
PID=$(pidof rknn_multi_stream)

# 常驻内存 / 峰值 / 线程数 / 句柄数, 每 5 秒刷新
watch -n 5 "grep -E 'VmRSS|VmHWM|Threads' /proc/$PID/status; echo fd: \$(ls /proc/$PID/fd | wc -l)"

# 硬件相关内存: CMA 可用量、DMA-BUF 总量(需要 root + debugfs)
grep -iE 'MemAvailable|Cma' /proc/meminfo
sudo tail -n 2 /sys/kernel/debug/dma_buf/bufinfo        # 末尾有 "Total N objects, M bytes"

# 长时间记录, 便于画曲线: 每分钟一行
while true; do echo "$(date +%T) $(grep VmRSS /proc/$PID/status)"; sleep 60; done >> rss.log
```

> 判断标准：**跑够时间**（至少覆盖几次重连、几轮文件循环、几次点播/挂断），**看趋势不看绝对值**。启动后几分钟内涨是正常的（缓冲填满、线程池建立、分配器预留）。

#### 12.4.2 普通堆泄漏：ASan / LSan

```bash
# 编译 (板端 GCC 同样支持; Debian/Ubuntu 需要安装 libasan)
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -O1" \
         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
make -j8

# 运行一段时间后 Ctrl+C 正常退出, LeakSanitizer 在进程结束时报告泄漏
ASAN_OPTIONS=detect_leaks=1:halt_on_error=0 ./rknn_multi_stream -c config/app.ini
```

- ASan 同时能抓**越界、释放后使用(UAF)、重复释放**，这些往往比泄漏更致命。
- **必须优雅退出**（Ctrl+C），`kill -9` 不会生成泄漏报告。这也是本项目坚持"所有线程都能 join、所有资源都在析构中释放"的原因之一。
- 本项目在 PC 上用桩库跑 4 路 + 2 模型 + 2 路推流，ASan/LSan 零报告，可以作为你修改代码后的回归检查。

#### 12.4.3 普通堆泄漏：heaptrack / valgrind

```bash
# heaptrack: 开销小, 适合长时间跑, 能看"哪一行分配的内存最后没释放"、峰值由谁造成
heaptrack ./rknn_multi_stream -c config/app.ini
heaptrack_print heaptrack.rknn_multi_stream.*.zst | less      # 或 heaptrack_gui 图形界面

# valgrind memcheck: 精确但慢 20~50 倍, 适合短时间跑 PC 桩库版本
valgrind --leak-check=full --show-leak-kinds=definite ./rknn_multi_stream -c test.ini

# valgrind massif: 看内存随时间的构成(谁占得最多)
valgrind --tool=massif ./rknn_multi_stream -c test.ini && ms_print massif.out.*
```

> 在板子上对 MPP/RGA/NPU 用 valgrind 容易出现大量误报（它不理解驱动的 ioctl/mmap），推荐在 **PC 桩库版本**上查普通堆问题，在板子上用 ASan。

#### 12.4.4 硬件内存泄漏：堆工具看不到的那部分

MPP 的解码/编码 buffer、RGA 的导入句柄、NPU 的权重和中间结果都不走 malloc，ASan/valgrind **看不到**。只能靠"成对释放"的纪律和系统计数来查：

```bash
grep -i cma /proc/meminfo                          # CmaFree 持续下降 -> 可疑
sudo tail -n 2 /sys/kernel/debug/dma_buf/bufinfo   # DMA-BUF 对象数/总字节数持续上涨 -> 可疑
export mpp_buffer_debug=1                          # MPP 通过环境变量读取调试掩码, 跟踪 buffer 分配/释放
export ROCKCHIP_RGA_LOG=1                          # 打开 librga 日志 (也可用 ROCKCHIP_RGA_LOG_LEVEL 调级别)
```

#### 12.4.5 本项目的资源"申请/释放"对照表

代码审查时逐项核对，**每一个申请都必须在所有路径（包括错误路径、重连路径、退出路径）上有对应的释放**：

| 资源 | 申请 | 释放 | 位置 |
|---|---|---|---|
| AVFormatContext | `avformat_alloc_context` / `avformat_open_input` | `avformat_close_input`（open 失败时 FFmpeg 已自行释放，要把指针置空） | `StreamLoader::openInput/closeInput` |
| AVBSFContext | `av_bsf_alloc` | `av_bsf_free` | `StreamLoader::closeInput` |
| AVPacket | `av_packet_alloc`、`av_read_frame` 填充引用 | 每次用完 `av_packet_unref`，最后 `av_packet_free` | `StreamLoader::pump` |
| AVCodecParameters | `avcodec_parameters_alloc` | `shared_ptr` 自定义删除器 `avcodec_parameters_free` | `StreamLoader::openInput` |
| AVCodecContext / AVFrame / SwsContext | `avcodec_alloc_context3` / `av_frame_alloc` / `sws_getCachedContext` | `avcodec_free_context` / `av_frame_free` / `sws_freeContext` | `FfmpegDecoder`、`FfmpegEncoder` 析构 |
| AVFormatContext(推流) | `avformat_alloc_output_context2` / `avio_open2` | `av_write_trailer` → `avio_closep` → `avformat_free_context` | `FfmpegPushSink::close` |
| MppCtx | `mpp_create` + `mpp_init` | `mpi->reset` → `mpp_destroy` | `MppDecoder/MppEncoder::release` |
| MppBufferGroup | `mpp_buffer_group_get_internal` | `mpp_buffer_group_put`（**在 mpp_destroy 之后**） | 同上 |
| MppPacket（解码输入） | `mpp_packet_init` | `mpp_packet_deinit` | `MppDecoder::decode` |
| MppFrame（解码输出） | `decode_get_frame` | `mpp_frame_deinit`（**不释放就占着 buffer group 里的块，解码器会卡死**） | `MppDecoder::drainFrames` |
| MppBuffer（编码输入/header） | `mpp_buffer_get` | `mpp_buffer_put` | `MppEncoder::release` |
| MppPacket（编码输出） | `encode_get_packet` | `mpp_packet_deinit` | `MppEncoder::encode` |
| rknn_context | `rknn_init` / `rknn_dup_context` | `rknn_destroy`（先 dup 实例，后 master） | `~RknnLite`、`ModelManager::shutdown` |
| rknn 输出 buffer | `rknn_outputs_get` | `rknn_outputs_release`（**每帧必须配对，漏掉就是每帧 2MB 的泄漏**） | `RknnLite::interf` |
| RGA | `wrapbuffer_fd/virtualaddr`（不产生句柄） | 无需释放；若改用 `importbuffer_*` 则必须 `releasebuffer_handle` | `RgaUtils.cc` |
| socket | `socket()` | `close()`（重连/会话结束/析构都要关） | `Gb28181Sink` |
| 线程 | `WorkerThread::start` | 析构中 `join` | 各 Worker |
| cv::Mat | 引用计数 | 最后一个持有者释放 | Mbuffer 只保留 1 帧，队列有界 |

#### 12.4.6 "逻辑泄漏"：没有丢失指针，但内存一直涨

ASan 报不出来，但同样会把内存吃光：

- **无界容器**：队列不设上限、map 只增不删。本项目所有队列有界（包队列 64），Mbuffer 只存 1 帧，`FfmpegEncoder::pts_map_` 限制 64 项，PerfMonitor 的统计项名字是固定集合。
- **引用计数"挂住"大对象**：某个线程长期持有一个 `cv::Mat` 或 `shared_ptr<VideoPacket>` 不放。
- **下游停摆**：推流一直连不上时如果还在编码并缓存 → 内存上涨。本项目在退避期和 GB28181 未点播时**不编码**。
- 定位方法：heaptrack 的"峰值分配"视图、或在 perf 报告里临时加一行打印各队列 `size()`。

#### 12.4.7 缩小范围：用配置做二分

不改代码，只改配置，逐步关掉模块，看泄漏是否消失：

```
 1. [pushN] enable=0            -> 泄漏消失?  问题在推流/编码
 2. display=0                   -> 泄漏消失?  问题在 GUI (highgui 后端)
 3. use_rga=0                   -> 泄漏消失?  问题在 RGA 路径
 4. [decoder] backend=ffmpeg    -> 泄漏消失?  问题在 MPP 路径
 5. 只保留 1 路 source          -> 泄漏速度减为 1/4?  每路都在泄漏(按路分配的资源)
 6. 输入换成本地文件            -> 泄漏消失?  问题在网络重连路径
```

### 12.5 数据竞争：ThreadSanitizer

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1" \
         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
make -j8
TSAN_OPTIONS="halt_on_error=0 suppressions=tsan.supp" ./rknn_multi_stream -c test.ini
```

**误报处理**：FFmpeg 的帧线程、OpenCV `cv::Mat` 的引用计数是在**未插桩**的动态库里用原子指令同步的，TSan 看不到这些同步关系，会报"free 与之前的读写冲突"。本项目实测的 5 条报告全部属于这一类。可以写抑制文件 `tsan.supp`（示例）：

```
called_from_lib:libavcodec.so
called_from_lib:libavutil.so
called_from_lib:libopencv_core.so
```

**判断真假的方法**：看报告里**两个栈**的"另一边"是不是在我们自己的代码里、并且访问的是我们自己的成员变量。如果两边都落在第三方库的引用计数/释放逻辑里，大概率是误报。

### 12.6 死锁与卡死

```bash
PID=$(pidof rknn_multi_stream)
gdb -p $PID -batch -ex "thread apply all bt" > bt.txt     # 所有线程的调用栈
grep -n "Thread\|pthread_cond\|__lll_lock\|poll\|recv" bt.txt
```

看栈时重点关注：

- 卡在 `pthread_cond_wait`（**不带 timed**）的线程：它在等谁？本项目的无超时等待只有两处：`ModelManager::Lease` 等空闲实例、`inferAll` 里 `future.get()` 等推理结果。两者都依赖"推理任务本身能结束"，如果它们卡住，下一步就去看推理线程池是否卡在 `rknn_run` 里（NPU 驱动异常）。
- 卡在 `__lll_lock_wait`：两个线程互相持有对方要的锁？检查加锁顺序是否一致。
- 卡在 `poll`/`recv`/`connect`：网络调用有没有超时？FFmpeg 调用有没有设 `interrupt_callback` 截止时间？
- 退出卡住：是不是某个队列/Mbuffer 没有 `close()`，消费者还在 `pop()` 上等？

### 12.7 崩溃调试

```bash
# 保留符号的优化构建
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo && make -j8

# 允许生成 core
ulimit -c unlimited
echo '/tmp/core.%e.%p' | sudo tee /proc/sys/kernel/core_pattern

# 复现后分析
gdb ./rknn_multi_stream /tmp/core.rknn_multi_str.<pid>
(gdb) bt                    # 崩溃线程的栈
(gdb) info threads          # 看是哪个线程(线程名可见)
(gdb) thread apply all bt   # 所有线程
```

常见崩溃原因对照：

| 栈顶位置 | 可能原因 |
|---|---|
| `std::terminate` / `abort` | joinable 的 std::thread 被析构、线程里有未捕获异常（本项目 `set_terminate` 会先打日志） |
| `rknn_run` / `rknn_outputs_get` | 上下文已销毁还在用（退出顺序错）、多线程同时用一个上下文 |
| `mpp_*` | buffer group 已 put 但解码器还在用（释放顺序错） |
| `cv::Mat` 析构 / `free` | 重复释放、越界写坏了堆 → 用 ASan 复现 |
| `improcess` / `imcvtcolor` | 传入的 buffer 尺寸与实际分配不符 |
| 进程无栈直接消失 | 被 SIGPIPE 杀死（检查是否忽略了 SIGPIPE）、被 OOM killer 杀死（`dmesg \| grep -i oom`） |

### 12.8 流媒体调试

```bash
# 本地起一个 RTSP/RTMP 服务器(mediamtx, 单个可执行文件)
./mediamtx

# 验证推出去的流
ffprobe -v error -show_streams rtsp://127.0.0.1:8554/ch0
ffprobe -v error -show_streams rtmp://127.0.0.1:1935/live/mosaic
ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://127.0.0.1:8554/ch0   # 低延迟播放

# 解码一遍看有没有错误(没有输出 = 没有错误)
ffmpeg -v error -rtsp_transport tcp -i rtsp://127.0.0.1:8554/ch0 -t 10 -f null -

# 用 ffmpeg 模拟一个 RTSP 摄像头(作为输入源)
ffmpeg -re -stream_loop -1 -i test.mp4 -c copy -f rtsp rtsp://127.0.0.1:8554/src
```

**GB28181 抓包分析**：

```bash
sudo tcpdump -i any -w gb.pcap 'udp port 5060 or portrange 30000-30100'
```

用 Wireshark 打开：过滤 `sip` 看信令；Telephony → VoIP Calls → Flow Sequence 看时序图；媒体端口右键 Decode As → RTP。

**验证 PS 流是否正确**：把 RTP 负载拼起来（去掉每包 12 字节 RTP 头，TCP 模式还要去掉 2 字节长度前缀）保存为 `out.ps`：

```bash
ffprobe -v error -f mpeg -show_streams out.ps      # 应识别出 h264 及分辨率
ffmpeg  -v error -f mpeg -i out.ps -f null -       # 无输出 = 全部可解码
```

> 注意：抓包在帧中间停止时，最后一帧是残缺的，会报一次解码错误，这是正常的。本项目用模拟平台测试时就遇到过，用"二分帧"定位后确认只有最后一帧报错。

### 12.9 PC 上无板调试

没有板子时，可以在 x86 上跑完整流水线（编解码走 FFmpeg、图像走 OpenCV、NPU 用桩库）：

```bash
sudo apt install libopencv-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev ffmpeg

# 以下命令都在仓库根目录执行(标签文件 ./model/coco_80_labels_list.txt 按当前目录查找)

# 1. 编译桩库(把附录 C 的代码保存为 rknn_stub.cc)
g++ -shared -fPIC -O2 -Iinclude rknn_stub.cc -o librknnrt_stub.so

# 2. 编译主程序(关闭 MPP/RGA)
cmake -S . -B build-pc -DENABLE_MPP=OFF -DENABLE_RGA=OFF -DRKNN_RT_LIB=$PWD/librknnrt_stub.so
cmake --build build-pc -j8

# 3. 生成测试视频
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 10 -c:v libx264 -g 30 h264_720p.mp4
ffmpeg -f lavfi -i mandelbrot=size=640x480:rate=25 -t 10 -c:v libx265 hevc_480p.mp4

# 4. 运行(桩库不解析模型内容, 但模型文件必须存在且非空)
LD_LIBRARY_PATH=. ./build-pc/rknn_multi_stream model/RK3588/yolov5s-640-640.rknn h264_720p.mp4 hevc_480p.mp4
```

桩库会让每个模型输出一个移动的"person"框，足够验证线程、队列、融合、拼接、推流和退出流程。`build-pc/`、测试视频和桩库都不要提交（`.gitignore` 已忽略 `*.mp4`，`build-pc/` 和 `.so` 需要自己注意）。

### 12.10 常见问题速查

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 画面发绿/颜色错位 | NV12 的 UV 起点按 `width*height` 算了 | 用 `hor_stride*ver_stride`，按 stride 逐行拷贝 |
| 人脸发蓝/颜色互换 | RGB/BGR 搞反 | OpenCV 默认 BGR；RGA 的 `RK_FORMAT_RGB_888` 字节序是 R,G,B |
| 框整体偏移/偏大 | letterbox 的 pad/scale 没有反算 | `x = (x_model - pad) / scale`，再裁剪到原图 |
| 同类框重叠没去掉 / 不同类互相抑制 | NMS 下标混用 | 见 6.6 |
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

## 13. 练习题与学习路线

### 13.1 建议学习顺序

```
 第 1 天: 跑起来      PC 桩库版本跑通 4 路 + 推流到 mediamtx, 看 perf 报告
 第 2 天: 地基        BlockingQueue / Mbuffer / FpsController / WorkerThread / App 生命周期
 第 3 天: 输入        StreamLoader(超时、重连、GOP 丢包) + MppDecoder(时序、info_change)
 第 4 天: 推理        rknn_lite / ModelManager / postprocess(NMS) / letterbox 坐标映射
 第 5 天: 融合+输出   DetectionFusion + Compositor + StreamingMgr + 编码器
 第 6 天: 国标        GB28181 信令时序 + PS/RTP 封装, 用 Wireshark 看一遍真实报文
 第 7 天: 调试        ASan/TSan/heaptrack 各跑一次, 故意制造一个泄漏再把它找出来
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

---

## 附录 A：配置速查

| 段 | 关键项 | 默认 | 说明 |
|---|---|---|---|
| `[general]` | `log_level` | info | debug 会打印 SIP 报文 |
| | `perf_interval` | 5 | 性能报告周期(秒)，0 不打印 |
| | `display` | 1 | 无 DISPLAY 自动关闭 |
| | `use_rga` | 1 | 失败自动回退 OpenCV |
| `[sourceN]` | `url` | — | 文件 / rtsp:// / rtmp:// / /dev/videoN |
| | `loop` / `realtime` | 1 / 1 | 文件循环 / 按 pts 节奏读 |
| | `rtsp_transport` | tcp | tcp / udp |
| | `timeout_ms` / `reconnect_max_ms` | 5000 / 30000 | 超时 / 重连退避上限 |
| `[decoder]` | `backend` | auto | auto / mpp / ffmpeg |
| | `mpp_split_parse` | 1 | 0 可省一帧延迟 |
| | `packet_queue` | 64 | 拉流→解码 队列长度 |
| `[modelN]` | `path` / `labels` | — | 模型 / 标签 |
| | `instances` | 3 | 最大并发度 |
| | `core` | auto | auto / any / 0 / 1 / 2 / 0_1 / 0_1_2 |
| | `conf_thresh` / `nms_thresh` | 0.25 / 0.45 | 单模型阈值 |
| | `weight` / `letterbox` | 1.0 / 1 | 融合权重 / 等比缩放 |
| `[infer]` | `infer_interval` | 1 | 跳帧推理 |
| | `target_fps` | 25(示例) | 0 为不限 |
| | `reuse_max_ms` | 500 | 结果复用时效 |
| | `draw_model_boxes` | 0 | 调试：画各模型原始框 |
| `[fusion]` | `method` | weighted | nms / weighted / confidence |
| | `iou_thresh` / `nms_thresh` | 0.55 / 0.45 | 关联 / 融合后 NMS |
| | `score_thresh` / `min_votes` | 0.25 / 1 | 最低分 / 投票数 |
| `[mosaic]` | `width` / `height` / `fps` / `cols` | 1920 / 1080 / 25 / 0 | cols=0 自动 |
| `[pushN]` | `enable` / `type` | 0 / rtmp | rtmp / rtsp / gb28181 |
| | `source` | mosaic | mosaic 或通道号 |
| | `encoder` | auto | auto / mpp / ffmpeg / ffmpeg:libx264 |
| | `fps` / `bitrate_kbps` / `gop` | 25 / 4000 / 50 | 编码参数 |
| | GB28181 项 | 见 config/app.ini | sip_server_*, device_id, channel_id, password, local_sip_port, media_port, expires, keepalive, on_duplicate_invite |

## 附录 B：关键 API 速查

| 模块 | API | 本项目用途 |
|---|---|---|
| FFmpeg | `avformat_open_input` / `avformat_find_stream_info` / `av_read_frame` | 拉流读包 |
| | `av_bsf_*` (`h264_mp4toannexb`) | AVCC → Annex-B |
| | `AVIOInterruptCB` | 阻塞调用超时/中断 |
| | `avformat_alloc_output_context2` / `avformat_write_header` / `av_interleaved_write_frame` / `av_write_trailer` | RTMP/RTSP 推流 |
| MPP | `mpp_create` / `mpp_init` / `mpp_destroy` | 创建/销毁 |
| | `decode_put_packet` / `decode_get_frame` | 解码 |
| | `MPP_DEC_SET_EXT_BUF_GROUP` / `MPP_DEC_SET_INFO_CHANGE_READY` | info_change 处理 |
| | `mpp_enc_cfg_set_*` / `MPP_ENC_SET_CFG` / `MPP_ENC_SET_HEADER_MODE` / `MPP_ENC_GET_HDR_SYNC` | 编码配置 |
| | `encode_put_frame` / `encode_get_packet` / `MPP_ENC_SET_IDR_FRAME` | 编码 / 强制 IDR |
| RGA | `wrapbuffer_fd` / `wrapbuffer_virtualaddr` | 包装 buffer |
| | `imcvtcolor` | NV12↔BGR |
| | `improcess(src, dst, pat, srect, drect, prect, ...)` | 缩放 + 颜色转换 + 写入 ROI |
| RKNN | `rknn_init` / `rknn_dup_context` / `rknn_destroy` | 上下文 |
| | `rknn_set_core_mask` | 绑定 NPU 核 |
| | `rknn_query` (IN_OUT_NUM / INPUT_ATTR / OUTPUT_ATTR / PERF_DETAIL) | 查询属性/性能 |
| | `rknn_inputs_set` / `rknn_run` / `rknn_outputs_get` / `rknn_outputs_release` | 推理 |

## 附录 C：PC 无板调试用的 RKNN 桩库

模拟一个 YOLOv5（1 输入 3 输出、int8、NCHW），每次推理输出一个水平移动的 "person" 框；不同模型的框略有偏移，用于验证融合。`rknn_run` 里 sleep 12ms 模拟 NPU 耗时。

```cpp
// rknn_stub.cc —— 仅用于 PC 调试, 不要部署到板子上
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include "rknn_api.h"

static std::mutex g_mtx;
static std::map<rknn_context, int> g_model_of;   // ctx -> 模型编号
static rknn_context g_next = 1;
static int g_models = 0;

extern "C" {
int rknn_init(rknn_context *ctx, void *, uint32_t, uint32_t, rknn_init_extend *) {
    std::lock_guard<std::mutex> l(g_mtx); *ctx = g_next++; g_model_of[*ctx] = g_models++; return 0; }
int rknn_dup_context(rknn_context *in, rknn_context *out) {
    std::lock_guard<std::mutex> l(g_mtx); *out = g_next++; g_model_of[*out] = g_model_of[*in]; return 0; }
int rknn_destroy(rknn_context ctx) { std::lock_guard<std::mutex> l(g_mtx); g_model_of.erase(ctx); return 0; }
int rknn_set_core_mask(rknn_context, rknn_core_mask) { return 0; }
int rknn_query(rknn_context, rknn_query_cmd cmd, void *info, uint32_t) {
    if (cmd == RKNN_QUERY_SDK_VERSION) { auto *v = (rknn_sdk_version *)info; strcpy(v->api_version, "stub"); strcpy(v->drv_version, "stub"); return 0; }
    if (cmd == RKNN_QUERY_IN_OUT_NUM) { auto *n = (rknn_input_output_num *)info; n->n_input = 1; n->n_output = 3; return 0; }
    auto *a = (rknn_tensor_attr *)info;
    if (cmd == RKNN_QUERY_INPUT_ATTR) {
        a->n_dims = 4; a->dims[0] = 1; a->dims[1] = 640; a->dims[2] = 640; a->dims[3] = 3;
        a->fmt = RKNN_TENSOR_NHWC; a->type = RKNN_TENSOR_UINT8; return 0; }
    if (cmd == RKNN_QUERY_OUTPUT_ATTR) {
        int g = 80 >> a->index;
        a->n_dims = 4; a->dims[0] = 1; a->dims[1] = 255; a->dims[2] = g; a->dims[3] = g;
        a->fmt = RKNN_TENSOR_NCHW; a->type = RKNN_TENSOR_INT8; a->qnt_type = RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
        a->zp = -128; a->scale = 1.f / 255.f; return 0; }
    return -1;
}
int rknn_inputs_set(rknn_context, uint32_t, rknn_input[]) { return 0; }
int rknn_run(rknn_context, rknn_run_extend *) { std::this_thread::sleep_for(std::chrono::milliseconds(12)); return 0; }

static int8_t q(float v) { int x = (int)lroundf(v * 255.f) - 128; return (int8_t)(x < -128 ? -128 : x > 127 ? 127 : x); }

int rknn_outputs_get(rknn_context ctx, uint32_t n, rknn_output out[], rknn_output_extend *) {
    int model; { std::lock_guard<std::mutex> l(g_mtx); model = g_model_of[ctx]; }
    static std::atomic<int> tick{0}; int t = tick++;
    for (uint32_t k = 0; k < n; k++) {
        int g = 80 >> k; size_t sz = 255 * g * g;
        out[k].buf = malloc(sz); out[k].size = sz; memset(out[k].buf, (uint8_t)q(0.f), sz);
    }
    // 在 stride 32 的输出头(k=2)、anchor 0(116x90) 上放一个框
    int8_t *p = (int8_t *)out[2].buf; int g = 20, gl = g * g;
    float cx = 160 + (t % 300) + model * 8, cy = 320 + model * 6, w = 150, h = 200;
    int j = (int)(cx / 32), i = (int)(cy / 32);
    auto set = [&](int c, float v) { p[c * gl + i * g + j] = q(v); };
    set(0, ((cx / 32 - j) + 0.5f) / 2); set(1, ((cy / 32 - i) + 0.5f) / 2);
    set(2, sqrtf(w / 116) / 2);         set(3, sqrtf(h / 90) / 2);
    set(4, 0.9f);                       set(5, 0.9f);     // obj=0.9, class0(person)=0.9
    return 0;
}
int rknn_outputs_release(rknn_context, uint32_t n, rknn_output out[]) {
    for (uint32_t k = 0; k < n; k++) free(out[k].buf); return 0; }
}
```
