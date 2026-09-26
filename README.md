# 简介
* RK3588/RK3588S 上的 **多路视频流 + 多模型 NPU 推理 + 结果融合 + 拼接显示/推流** 工程, C++ 实现
* 拉流 FFmpeg → 解码 **MPP** 硬解 → 颜色转换/缩放 **RGA** → 推理 **RKNN**(多模型、按 NPU 核心绑定) → 多模型融合 → 拼接 → **MPP** 硬编 → RTMP / RTSP / **GB28181** 推流
* 每个环节都有 CPU 兜底(FFmpeg 软解/软编、OpenCV), 硬件接口失败时自动回退, 同一套代码也能在 PC 上编译调试
* 最初改自 [rknpu2](https://github.com/rockchip-linux/rknpu2) 的 yolov5 demo 与 [线程池](https://github.com/senlinzhan/dpool), python 版见 [rknn-multi-threaded](https://github.com/leafqycc/rknn-multi-threaded)

# 处理流程

```
输入: 4 路视频 / RTSP / MP4 数据源
 │
 ├─ s1 StreamLoader        avformat_open_input / av_read_frame          拉流 + 读包(H.264/H.265 -> Annex-B)
 │        │ BlockingQueue<VideoPacket>  (有界队列, 实时流溢出时按 GOP 丢包)
 ├─ s2 MppDecoder          mpp_create/mpp_init                          MPP 硬件解码 H.264/H.265 -> NV12
 │                         decode_put_packet / decode_get_frame / mpp_frame_get_xxx
 ├─ s3 mpp_decoder_cb      RGA(DMA-BUF) 或 拷贝 YUV + cv::cvtColor(YUV2BGR_NV12)
 │        │ 写入 Mbuffer.img  (最新帧槽, 覆盖写, 下游慢则自动丢旧帧)
 ├─ s4 rknn_infer 线程      从 Mbuffer 读 BGR 图 -> rknn_lite::interf
 │                         BGR->RGB + 缩放(RGA / cv::resize, letterbox)
 │                         rknn_inputs_set / rknn_run / rknn_outputs_get
 │                         post_process 解码 + NMS -> detect_result_group_t (g1..gN, 多模型并发)
 ├─ s5 DetectionFusion     fuseDetections(g1,g2,g3,g4)
 │                         calculateIoU / weightedFusion / confidenceFusion
 │                         applyNMS -> FusedDetection[] -> drawFusedDetections(ori_img)
 │        │ images[i] (每路结果 Mbuffer)
 └─ s6 输出                 Compositor::combineImage -> 多路拼图 -> cv::imshow(主线程)
                           StreamingMgr::streamingWorker 读 images[i]/拼图 -> 叠加时间戳/统计
                           -> MPP 硬编 H.264 -> RTMP / RTSP / GB28181(PS over RTP)
```

## 线程模型

| 线程 | 数量 | 职责 | 输入 → 输出 |
| ---- | ---- | ---- | ---- |
| `chN-demux` | 每路 1 | 拉流/读包, 断线重连 | 网络/文件 → 包队列 |
| `chN-dec` | 每路 1 | MPP 解码 + NV12→BGR | 包队列 → decoded Mbuffer |
| `chN-infer` | 每路 1 | 帧率控制、跳帧、提交推理、融合、画框 | decoded Mbuffer → images[i] |
| 推理线程池 | = 模型实例总数 | 多模型并发执行 `rknn_run` | 任务队列 → future |
| `mosaic` | 1 | 固定帧率拼接 | images[0..N] → mosaic Mbuffer |
| `pushN` | 每个推流项 1 | OSD + 编码 + 推流, 退避重连 | images[i]/mosaic → 网络 |
| `pushN-sip` | GB28181 每项 1 | 注册/心跳/目录查询/点播信令 | UDP 5060 |
| `perf` | 1 | 统计与瓶颈报告 | — |
| `main` | 1 | 信号处理、`cv::imshow` | mosaic Mbuffer |

线程名通过 `pthread_setname_np` 设置, `top -H -p <pid>` 可直接看到每个线程的 CPU 占用.

# 目录结构
```
config/app.ini                 配置示例(多路/多模型/融合/推流开关)
include/ src/
  app/        App(总装、初始化/退出顺序), main(信号、命令行)
  common/     Config, Logger, BlockingQueue, Mbuffer, FpsController, WorkerThread, PerfMonitor, RgaUtils, ThreadPool
  stream/     StreamLoader(FFmpeg 拉流)
  decode/     MppDecoder, FfmpegDecoder, DecodeWorker(mpp_decoder_cb)
  infer/      rknn_lite(RKNN 实例), ModelManager(实例池/核心绑定/并发推理), InferWorker, pre/postprocess
  fusion/     DetectionFusion
  output/     Compositor, StreamingMgr, MppEncoder, FfmpegEncoder, FfmpegPushSink(RTMP/RTSP), Gb28181Sink
    gb28181/  SipMessage, PsMuxer, Md5
```

# 编译与运行

### 依赖(板端)
```bash
sudo apt install libopencv-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev
sudo apt install librockchip-mpp-dev        # MPP 硬件编解码(没有则自动回退 FFmpeg 软件编解码)
```
* `librknnrt.so`(include/)与 `librga.so`(include/3rdparty/rga)已随仓库提供
* CMake 选项: `-DENABLE_MPP=ON/OFF`, `-DENABLE_RGA=ON/OFF`, `-DMPP_ROOT=<mpp 安装目录>`, `-DRKNN_RT_LIB=<librknnrt.so>`

### 运行
* 下载 Releases 中的测试视频到项目根目录, 运行 `build-linux_RK3588.sh`
* 可切换至 root 用户运行 `performance.sh` 定频, 提高性能和稳定性
* 进入 `install/rknn_multi_stream_Linux` 后:
```bash
# 快速运行(兼容旧用法): 单模型, 可跟多路输入, 摄像头序号 0 即 /dev/video0
./rknn_multi_stream ./model/RK3588/yolov5s-640-640.rknn ../../720p60hz.mp4 [源2 源3 源4]
# 完整功能: 多路 + 多模型融合 + 推流
./rknn_multi_stream -c config/app.ini
```
* 窗口中按 `q`/`Esc` 退出; 无显示环境(SSH/后台服务)自动以无窗口模式运行, `Ctrl+C` / `kill` 优雅退出

# 设计要点

## 1. 多路流处理架构
**解耦方式**: 每路 `拉流 → 解码 → 推理` 三个线程, 之间用两种缓冲区连接, 共享的只有模型池和输出线程:

| 缓冲 | 实现 | 语义 | 用在哪里 |
| ---- | ---- | ---- | ---- |
| `BlockingQueue<T>` | 有界 deque + mutex + 2 个条件变量 | 不丢数据 / 背压, `tryPush` 由调用方决定丢弃策略 | 拉流→解码(压缩包不能随意丢, 否则花屏) |
| `Mbuffer` | 单槽 + seq + 条件变量 | 覆盖写, 读者总是拿最新帧 | 解码→推理、推理→拼接/推流(实时优先) |

**生产者-消费者/互斥锁的工程写法**(见 `common/BlockingQueue.hpp`, `common/Mbuffer.hpp`):
* 条件变量一律用带谓词的 `wait(lock, pred)`, 防虚假唤醒; 等待都带超时或可被 `close()` 唤醒, 保证退出时不会有线程永久阻塞;
* 锁内只做指针/引用计数级别的操作, 不在锁内做拷贝、编码、网络 IO; `cv::Mat` 以引用计数浅拷贝跨线程传递, 约定"发布后不再修改", 需要修改(叠加 OSD)的消费者自行 `clone`;
* 统计量用 `std::atomic`(relaxed)累加, 热路径无锁.

## 2. MPP / RGA / RKNN 实战
* **MPP 解码**(`decode/MppDecoder.cc`): `mpp_create → mpp_init → MPP_DEC_SET_CFG(split_parse)`; 首帧的 `info_change` 中按 `buf_size` 创建外部 DRM buffer group 并 `limit_config` 限制数量, 然后 `MPP_DEC_SET_INFO_CHANGE_READY`; 输入队列满时先取帧再重试 `decode_put_packet`. 注意 **NV12 的 UV 平面起始于 `hor_stride * ver_stride`**(1080p 的 ver_stride 通常为 1088), CPU 路径拷贝时逐行去除 stride.
* **FFmpeg → MPP**: MP4/FLV 中是 AVCC 格式, 必须经过 `h264_mp4toannexb`/`hevc_mp4toannexb` 转为 Annex-B 并带内插入 SPS/PPS 才能送给 MPP.
* **RGA**(`common/RgaUtils.cc`): 解码输出直接用 DMA-BUF fd 做 NV12→BGR(零拷贝); 推理前处理一次 `improcess` 完成缩放 + BGR→RGB + letterbox; 编码前 BGR→NV12 直接写入 MPP buffer. 任何一步失败(对齐要求、>4G 内存等)自动回退 OpenCV 并只告警一次.
* **RKNN 多模型并行 + 按 NPU core 绑定**(`infer/ModelManager.cc`): 每个模型第一个实例 `rknn_init`, 其余实例 `rknn_dup_context` 共享权重; `core=auto` 时所有模型的实例在 core0/1/2 上轮询 `rknn_set_core_mask`, 保证三核负载均衡; 也可指定 `0_1_2` 让大模型独占三核. 实例池保证一个上下文同一时刻只被一个线程使用, 实例数 = 该模型并发度.

## 3. 实时系统优化思路
* **跳帧推理、结果复用**: `infer_interval=N` 每 N 帧推理一次, 其余帧直接绘制最近一次融合结果; `reuse_max_ms` 限制结果时效, 避免目标离开后残留旧框. 显示/推流帧率因此不受 NPU 限制.
* **目标帧率控制**(`common/FpsController.hpp`): 推理线程用"丢帧式" `accept()`(超出 `target_fps` 的帧直接丢弃), 拼接/推流线程用"节拍式" `wait()`; 两者都以绝对时间点累加周期, 不累积 sleep 误差, 落后过多时重新对齐而不是追帧.
* **线程池并发提交与等待**: 同一帧对 N 个模型 `pool->submit()` 得到 N 个 `future`, 再逐个 `get()` 等待(任务引用了调用方栈上的图像, 必须全部完成才返回). 线程数 = 实例总数, 持有实例的线程不再等待其它资源, 不会死锁. 单模型时直接在推理线程执行, 省一次线程切换.
* **性能监控与瓶颈定位**(`common/PerfMonitor.cc`): 每个阶段记录 频率 / 平均耗时 / 最大耗时 / 负载(= 频率 × 平均耗时, 即占用一个线程的比例), 负载 > 85% 的阶段会被标记为瓶颈; 同时统计各级丢帧: `chN.drop.pkt` 增长说明解码跟不上, `chN.drop.frame` 增长说明推理跟不上, `chN.drop.fps` 是目标帧率主动丢弃. 以 root 运行时还会打印 `/sys/kernel/debug/rknpu/load` 的 NPU 负载.

## 4. 检测结果融合
* **多模型结果关联**(`fusion/DetectionFusion.cc`): 所有框按 `分数 × 模型权重` 降序, 贪心地分配给 IoU 最大且 > `iou_thresh` 的同类簇; **同一簇中每个模型最多贡献一个框**(模型内部已做过 NMS, 同一模型的两个框一定是不同目标). 簇内融合方式:
  * `weighted`: WBF 加权框融合, 置信度按"检出模型数 / 总模型数"折算;
  * `confidence`: 取最高分框, 置信度 noisy-OR `1 - Π(1 - s_i)`, 多模型一致时提升;
  * `nms`: 只保留簇内最高分框.
* **NMS 的正确用法**: 先按置信度排序, 高分框只抑制与它 IoU 超过阈值的 **同类** 低分框. 原 `postprocess.cc` 的 NMS 用排序后的位置去索引类别数组, 且没有检查被抑制框的类别, 会出现跨类别误抑制、同类重复框漏抑制, 已修复; 原实现的全局标签数组与首次加载标志在多线程下存在竞争, 也改为每个模型实例独立持有. 融合后再做一次 `applyNMS` 去除相邻簇之间的残留重叠框.
* **误检/漏检权衡**: `min_votes=1` 取并集(高召回、误检多), `min_votes=模型数` 取交集(高精度、漏检多); 配合每个模型的 `conf_thresh`、融合的 `score_thresh` 与模型 `weight` 调整.

## 5. 工程稳定性与容错
* **初始化顺序对稳定性的影响**(`app/App.hpp`): 先加载模型(最耗时、最容易失败, 失败时还没有任何线程与网络连接) → 创建全部缓冲 → 先启动消费者(推流/拼接/推理/解码) → 最后启动生产者(拉流). 退出顺序相反: 先置停止标志并 `close()` 所有队列唤醒阻塞者, 再按生产者→消费者顺序 `join`, 最后才销毁 NPU 上下文(先 dup 实例后 master).
* **线程生命周期管理(避免 terminate)**: `std::thread` 析构时仍 joinable 会直接 `std::terminate`, 线程函数逃逸的异常也会 `terminate`. `WorkerThread` 在析构中 join, 并在线程入口统一 catch 异常; 所有阻塞调用(队列等待、`av_read_frame`、网络写)都带超时或可被中断(FFmpeg `interrupt_callback`). 另外忽略 `SIGPIPE`, 否则推流对端断开时进程会被直接杀死.
* **端口占用、重连、重复会话处理**:
  * 拉流: 打开/读包超时即重连, 指数退避(上限 `reconnect_max_ms`), 重连后码流参数变化会自动重建解码器;
  * 推流: 写失败先 `av_write_trailer`/TEARDOWN 关闭旧连接(不在服务器上残留自己的重复会话), 再退避重连, 重连首帧强制 IDR; 服务器因"该路径已有推流者"拒绝时给出明确日志; 配置加载时拒绝两个推流项使用同一 URL;
  * GB28181: 本地 SIP 端口被占用自动顺延(注意 UDP 不能设 `SO_REUSEADDR`, 否则冲突会"静默"发生), 媒体端口被占用回退系统分配; 心跳连续无应答判定平台掉线, 用新的 Call-ID 重新注册; 同 Call-ID 的 INVITE 重传原样回复缓存的 200 OK, 推流中收到新点播按 `on_duplicate_invite` 替换(先 BYE 旧会话)或回 486.

## 6. 推流协议链路
* **开关化配置**: 每个 `[pushN]` 独立开关(`enable`)、协议(`type = rtmp | rtsp | gb28181`)、源(`source = mosaic | 通道号`)、分辨率/帧率/码率/编码器, 可同时推多路, 互不影响.
* **处理结果如何进入推流**: 推理线程把画好框的帧写入 `images[i]`, 拼接线程写入 `mosaic`; `streamingWorker` 按自己的帧率从 Mbuffer 取最新帧 → clone 后叠加时间戳/统计 → `VideoEncoder`(MPP 硬编, SPS/PPS 保证随每个 IDR 带内发送) → `IStreamSink`:
  * RTMP: FLV 封装; RTSP: ANNOUNCE/RECORD(TCP/UDP 可选);
  * GB28181: REGISTER(Digest) + 心跳 + Catalog/DeviceInfo/DeviceStatus 应答 → 平台 INVITE → 200 OK(SDP) → ACK → H.264 封装为 **PS** → **RTP**(PT=96, SSRC 取平台 `y=` 值) 经 UDP / TCP(RFC4571, 主动或被动)发送; 未点播时不编码.

# 多线程模型帧率测试(单模型线程池, 历史数据)
* 使用 performance.sh 进行 CPU/NPU 定频尽量减少误差
* 测试模型: [yolov5s-relu](https://github.com/rockchip-linux/rknpu2/tree/master/examples/rknn_yolov5_demo/model/RK3588)

|  模型\线程数   | 1    |  2   | 3  |  4  | 5  | 6  | 9  | 12  |
|  ----  | ----  |  ----  | ----  |  ----  | ----  | ----  | ----  | ----  |
| Yolov5s - relu  | 41.6044 | 71.6037 | 98.6057 | 98.0068 | 104.6001 | 114.7454 | 129.5693 | 140.8788 |

# 补充
* 后处理针对 YOLOv5(1 输入 3 输出, int8 量化, NCHW), 类别数由输出张量形状自动推导
* PC 调试: `cmake -DENABLE_MPP=OFF -DENABLE_RGA=OFF -DRKNN_RT_LIB=<x86 版或桩库>` 即可在 x86 上编译, 编解码走 FFmpeg, 图像处理走 OpenCV

# Acknowledgements
* https://github.com/rockchip-linux/rknpu2
* https://github.com/rockchip-linux/mpp
* https://github.com/airockchip/librga
* https://github.com/senlinzhan/dpool
* https://github.com/ultralytics/yolov5
* https://github.com/airockchip/rknn_model_zoo
