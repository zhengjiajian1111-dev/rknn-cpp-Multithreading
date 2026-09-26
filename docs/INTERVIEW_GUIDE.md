# 面试准备手册（Linux 应用开发方向）

拿 rknn-cpp-Multithreading 这个项目去面 **Linux 应用 / 嵌入式 Linux 应用 / 音视频应用** 岗位时，面试官最可能问的问题和参考回答，全部整理在这一个文件里。

> 配套阅读：[LEARNING_GUIDE.md](LEARNING_GUIDE.md)（全链路图解 + 调试方法）。这里侧重"怎么答"，那边侧重"怎么懂"。

## 目录

- [面试官一般怎么考](#面试官一般怎么考)
- [面试前必须完成的 5 件事](#面试前必须完成的-5-件事)
- [高频问题 Top 20](#高频问题-top-20按被问概率排序)
- [Part 1 项目介绍怎么说](#part-1-项目介绍怎么说)
- [Part 2 项目深挖问答](#part-2-项目深挖问答)
- [Part 3 Linux / C++ 基础八股（结合项目）](#part-3-linux--c-基础八股结合项目)
- [Part 4 手撕代码](#part-4-手撕代码)
- [Part 5 "你遇到过最难的问题是什么"——踩坑故事](#part-5-你遇到过最难的问题是什么踩坑故事)
- [Part 6 项目不足与改进方案](#part-6-项目不足与改进方案)
- [Part 7 上板实测清单](#part-7-上板实测清单)

| 部分 | 内容 | 什么时候看 |
|---|---|---|
| Part 1 | 30 秒 / 1 分钟 / 3 分钟项目介绍、简历写法 | 最先背熟 |
| Part 2 | 项目深挖问答（架构、多线程、音视频、推理、推流、国标、稳定性、扩展） | 重点，反复过 |
| Part 3 | 和项目强相关的 Linux / C++ 八股，每题附"结合项目怎么说" | 刷题 |
| Part 4 | 手撕代码：阻塞队列、线程池、交替打印、shared_ptr、LRU、NMS、位操作等（均已编译测试） | 动手写，每题限时 |
| Part 5 | "遇到过什么难题"——踩坑故事（STAR 格式），标注了哪些是真实发生的 | 背熟 3 个 |
| Part 6 | 项目不足与改进方案、反问环节 | 面试前一天 |
| Part 7 | 上板实测清单和数据记录表 | **上板后立刻做** |

## 面试官一般怎么考

```
 项目深挖  ██████████████████████████  约一半时间   从简历最亮的点往下挖, 挖到你答不上来为止
 基础八股  ███████████████             约三成       多线程 / 网络 / 内存 / 信号 / C++, 常用项目举例追问
 手撕代码  ██████████                  约两成       生产者消费者、线程池、字符串/链表、位操作
```

（比例是经验值，不同公司差异很大。）

## 面试前必须完成的 5 件事

- [ ] **上板实测**：每路帧率、NPU 三核占用、各线程 CPU、端到端延迟、内存曲线（见 Part 7）。**没有实测数字，一问就露馅。**
- [ ] **能讲清每一处设计的"为什么"**：面试官会随机指一段代码问你为什么这么写。
- [ ] **跑一次 ASan 并截图**：证明"没有内存泄漏"不是空口说。
- [ ] **（强烈建议）用 WVP-PRO + ZLMediaKit 实际对接一次 GB28181**，截图平台上的画面。目前只和自写的模拟平台联调过。
- [ ] **融合效果**：要么在数据集上测一组对比（单模型 vs 融合），要么准备好诚实的说法（"重点是工程框架，融合策略可配置，还没系统评测"）。**不要编数字。**

## 高频问题 Top 20（按被问概率排序）

1. 项目整体架构讲一下，有几个线程，线程之间怎么通信？ → [Part 2 A1-A3](#a-架构与多线程)
2. 手写一个生产者消费者 / 阻塞队列。 → [Part 4 题 1](#part-4-手撕代码)
3. 下游处理不过来怎么办？为什么用两种缓冲区？ → [Part 2 A4](#a-架构与多线程)
4. 条件变量为什么要用 while / 谓词？ → [Part 3 1.2](#part-3-linux--c-基础八股结合项目)
5. 板子上实际能跑多少帧？NPU 利用率多少？延迟多少？ → [Part 7](#part-7-上板实测清单)
6. 内存泄漏怎么排查？ → [Part 2 E1](#e-稳定性与调试)
7. 程序怎么安全退出？std::thread 析构会发生什么？ → [Part 2 A7](#a-架构与多线程)
8. 死锁的条件，你的线程池为什么不会死锁？ → [Part 2 A6](#a-架构与多线程)
9. I/P/B 帧、GOP、SPS/PPS；为什么丢包要丢到关键帧？ → [Part 2 B1-B2](#b-音视频与-rockchip-硬件)
10. NV12 格式和 stride；零拷贝是什么？ → [Part 2 B4-B6](#b-音视频与-rockchip-硬件)
11. SIGPIPE 是什么？推流断开进程为什么会退出？ → [Part 2 D4](#d-推流与-gb28181)
12. select / poll / epoll 的区别。 → [Part 3 3.1](#part-3-linux--c-基础八股结合项目)
13. NPU 三个核怎么用起来的？ → [Part 2 C1-C2](#c-推理与融合)
14. RTSP 交互流程；RTP over TCP 和 UDP 的区别。 → [Part 2 D1](#d-推流与-gb28181)
15. GB28181 注册和点播流程。 → [Part 2 D5-D6](#d-推流与-gb28181)
16. 手写线程池。 → [Part 4 题 2](#part-4-手撕代码)
17. 遇到过最难的问题是什么？ → [Part 5](#part-5-你遇到过最难的问题是什么踩坑故事)
18. 如果要支持 16 / 32 路怎么改？ → [Part 2 F1](#f-扩展与设计权衡)
19. malloc 的原理，RSS 和 VSZ 的区别。 → [Part 3 4.x](#part-3-linux--c-基础八股结合项目)
20. 项目有什么不足？ → [Part 6](#part-6-项目不足与改进方案)

---

## Part 1 项目介绍怎么说

> 原则：**先讲清"做了什么、解决什么问题"，再讲"用了什么技术"，最后给"数据"**。所有数字必须是你自己测过的——下面用 `【上板实测】` 标出的地方，请用 Part 7 里测到的数据替换。

### 30 秒版（自我介绍里带一句）

> 我做过一个基于 RK3588 的多路视频智能分析项目：同时接入 4 路摄像头或视频流，用 MPP 硬件解码、RGA 做图像处理、NPU 做多模型并行目标检测，把多个模型的结果融合后拼接显示，并支持 RTMP、RTSP、GB28181 三种方式推流。我主要负责整体的多线程架构、推理调度和推流模块。

### 1 分钟版（"介绍一下你的项目"）

> 这个项目跑在 RK3588 上，目标是**多路视频的实时目标检测**。
>
> 链路是这样的：每一路视频先用 FFmpeg 拉流，交给 MPP 硬件解码成 NV12，再用 RGA 转成 BGR；然后送到 NPU 推理，NPU 有三个核，我把多个模型实例轮流绑定到三个核上并行推理；多个模型的检测结果做关联融合，画框后拼成一个多路画面，本地显示，同时编码推流，支持 RTMP、RTSP 和国标 GB28181。
>
> 工程上我重点解决了三个问题：一是**多线程解耦**，每路拉流、解码、推理分开，压缩码流用有界队列、图像用"只留最新帧"的缓冲，保证下游慢的时候延迟不会累积；二是**实时性**，做了目标帧率控制、跳帧推理和结果复用；三是**稳定性**，断流自动重连、推流断线重连、线程安全退出，并且用 ASan 做过内存泄漏检查。
>
> 最终在板子上 4 路 1080p 能做到每路 `【上板实测】` fps，NPU 三个核占用分别是 `【上板实测】`，端到端延迟大约 `【上板实测】` 毫秒。

### 3 分钟版（面试官说"详细讲讲"）

按这个顺序讲，每段 20~30 秒，边讲边可以在纸上画 [LEARNING_GUIDE 1.2 节](LEARNING_GUIDE.md#12-全链路图) 那张图：

1. **背景和目标**（为什么做）：多路摄像头需要同时做检测，单路单线程的 demo 撑不住，而且没有推流和对接平台的能力。
2. **整体链路**：拉流 → 解码 → 转换 → 推理 → 融合 → 拼接/推流，一句话带过每一段用的技术。
3. **线程模型**：每路 3 个线程，加推理线程池、拼接线程、推流线程；线程之间用两种缓冲区，讲清楚为什么。
4. **硬件用法**：MPP 解码的 stride 和 buffer 管理、RGA 零拷贝、NPU 多实例共享权重 + 绑核。
5. **实时性和稳定性**：帧率控制、跳帧、按 GOP 丢包；超时、重连、安全退出、SIGPIPE。
6. **结果**：`【上板实测】` 的帧率、延迟、NPU 占用；ASan 无泄漏；长时间运行 `【上板实测】` 小时稳定。
7. **不足和下一步**（主动说，加分）：见 Part 6。

### 面试官追问"你负责哪部分"

- **如实说**。如果整个项目是你自己完成的，就说"整体架构和各模块都是我写的，参考了 Rockchip 官方示例（rknpu2、MPP、RGA）的接口用法"。
- 如果被问到某个模块答不上来，比"说是自己写的但讲不清"要好的回答是："这块我主要参考了官方示例的用法，原理层面我理解的是……，细节我需要回去再确认"。

### 简历上的项目描述（参考写法）

```
RK3588 多路视频智能分析系统                                     C++ / Linux / RK3588
- 基于 FFmpeg + MPP + RGA + RKNN 实现 4 路视频"拉流-硬解-图像处理-NPU 推理-融合-推流"全链路,
  4 路 1080p 每路 【上板实测】 fps, 端到端延迟 【上板实测】 ms
- 设计多级生产者-消费者架构: 码流有界队列(背压/按 GOP 丢包) + 图像最新帧缓冲, 下游变慢时延迟不累积;
  目标帧率控制 + 跳帧推理 + 结果复用
- NPU 多模型并行: rknn_dup_context 共享权重, 实例轮询绑定 3 个 NPU 核, 线程池并发提交与等待;
  多模型结果关联 + WBF 加权融合 + 投票过滤; 修复原后处理 NMS 类别下标混用问题
- 推流支持 RTMP / RTSP / GB28181(自研 SIP 注册/心跳/点播 + PS 封装 + RTP over UDP/TCP), 配置化开关;
  断流/断线指数退避重连, 关键帧门控, 重复会话处理
- 稳定性: 统一线程生命周期管理避免 std::terminate, 有序初始化/退出; ASan/TSan 检查;
  内置分阶段性能统计定位瓶颈
```

**注意**：简历上写的每一个词，面试官都可能追问。写不出实测数字就不要写数字；GB28181 如果没有和真实平台对接过，面试时要能说清楚"和模拟平台联调过，信令和 PS 流用 ffprobe 验证过"。

### 技术栈一句话

> C++14、CMake、pthread / std::thread；FFmpeg（拉流、封装）；Rockchip MPP（硬件编解码）、RGA（2D 加速）、RKNN（NPU 推理）；OpenCV（图像）；自研 GB28181（SIP / PS / RTP）；调试用 gdb、ASan、TSan、perf。


---

## Part 2 项目深挖问答

> 格式：**问** → **答**（可以直接说的版本）→ **追问**（面试官可能接着问的）→ **代码**（出处，被追问细节时回去对照）。
> 答案里的数字都来自代码配置；**性能数字一律以你上板实测为准**（见 Part 7）。

**本部分目录：**
- [A 架构与多线程](#a-架构与多线程)
- [B 音视频与 Rockchip 硬件](#b-音视频与-rockchip-硬件)
- [C 推理与融合](#c-推理与融合)
- [D 推流与 GB28181](#d-推流与-gb28181)
- [E 稳定性与调试](#e-稳定性与调试)
- [F 扩展与设计权衡](#f-扩展与设计权衡)

---

### A 架构与多线程

#### A1 整体架构讲一下？
**答：** 一条六段的流水线。每一路视频：FFmpeg 拉流读包 → MPP 硬件解码成 NV12 → RGA 转成 BGR → NPU 推理（多模型并行）→ 多模型结果融合、画框 → 结果交给拼接线程拼成多路画面，本地显示，同时推流线程编码后推 RTMP / RTSP / GB28181。路数、模型、融合方式、推流开关都在 INI 配置里。

**代码：** `src/app/App.cc`（总装），图见 `docs/LEARNING_GUIDE.md` 1.2 节。

#### A2 一共几个线程？各做什么？
**答：** 每路 3 个：拉流线程 `chN-demux`、解码线程 `chN-dec`、推理线程 `chN-infer`；全局有：推理线程池（线程数 = 所有模型实例数之和，只在多模型时使用）、拼接线程 `mosaic`、每个推流项一个 `pushN`、GB28181 推流项再加一个 SIP 线程、统计线程 `perf`、主线程（信号处理和 `imshow`）。4 路 1 模型、开 1 路推流时大约 12 + 1 + 1 + 1 + 1 = 16 个线程。线程都有名字，`top -H` 能直接看每个线程的 CPU。

**追问：** 线程这么多，上下文切换开销大不大？
→ 大部分线程在条件变量上等待，不占 CPU；真正忙的是解码回调里的颜色转换、推理等待和编码。路数很多时可以改成少量工作线程 + 事件驱动（见 F1/F3）。

#### A3 线程之间怎么通信？
**答：** 三种方式：
1. **有界阻塞队列 `BlockingQueue`**：拉流 → 解码，传压缩码流；
2. **最新帧缓冲 `Mbuffer`**：解码 → 推理 → 拼接/推流，传图像，只保留最新一帧；
3. **`std::future`**：推理线程把任务提交到线程池后，用 future 等结果。

控制类信息（停止标志、GB28181 的"媒体链路断开"）用 `std::atomic`。

**代码：** `include/common/BlockingQueue.hpp`、`include/common/Mbuffer.hpp`、`src/infer/ModelManager.cc`。

#### A4 下游处理不过来怎么办？为什么要两种缓冲区？
**答：** 核心原则是"**码流不能随便丢，图像只要最新的**"。
- 压缩码流：P 帧依赖前面的帧，随便丢一个包后面整组画面都会花屏，所以用有界队列。实时流满了就把积压的整个 GOP 清掉、等下一个关键帧再继续；文件源满了就阻塞等待（背压），一帧不丢。
- 解码后的图像：每帧几 MB，排队只会让延迟和内存累积，所以用单槽覆盖——下游慢，中间帧自动被覆盖丢弃，读者永远拿最新的。

每一级都有明确的丢弃策略，并且丢弃都会计数（`chN.drop.pkt`、`chN.drop.frame`、`chN.drop.fps`），看哪一级开始丢，就知道瓶颈在它的下游。

**追问：** 队列容量 64 怎么定的？
→ 默认 64 个包，在 25~30fps 下大约是 2 秒的码流：足够吸收网络抖动和关键帧解码变慢，又不会让延迟无限累积。它是经验值，可以在 `[decoder] packet_queue` 调；上板后可以根据 `drop.pkt` 计数和延迟要求调整。

#### A5 为什么拉流和解码拆成两个线程？
**答：** 解耦 IO 和计算。拉流是网络 IO，可能因为抖动阻塞几百毫秒；解码是计算，可能因为关键帧变慢。放在一起的话，网络抖动会让解码停顿，解码变慢又会让 socket 接收缓冲区溢出丢包。拆开后中间的队列可以吸收双方的抖动。

#### A6 死锁的条件是什么？你的线程池为什么不会死锁？
**答：** 死锁四个必要条件：互斥、占有并等待、不可剥夺、循环等待，破坏任意一个就不会死锁。

线程池里的任务要先"租"一个模型实例（`Lease`）。我让线程池线程数 = 所有模型实例数之和，并且每个任务只持有**一个**实例、持有期间不再等待任何其它资源，破坏了"占有并等待"。最坏情况是某模型实例全被占用，其它任务排队，但持有者一定能跑完并归还，所以一定能推进。

**追问：** 项目里还有哪里可能卡住？
→ 无超时的等待只有两处：`Lease` 等空闲实例、`future.get()` 等推理结果，都依赖推理本身能结束。如果 NPU 驱动卡死，它们会跟着卡。其余等待都带超时或能被 `close()` 唤醒。

**代码：** `src/infer/ModelManager.cc`（`Lease`、`init` 里线程数的注释）。

#### A7 程序怎么安全退出？`std::thread` 析构会发生什么？
**答：** `std::thread` 析构时如果还是 joinable，会直接调用 `std::terminate`；线程函数里逃逸出的异常也会 `terminate`。我用 `WorkerThread` 封装：析构时 join，线程入口统一 `try/catch`。

退出流程分三步：
1. **通知**：给所有模块置停止标志，并 `close()` 所有队列和 Mbuffer，把阻塞在 `pop()`/`waitNew()` 上的线程全部唤醒；FFmpeg 的阻塞调用靠 `interrupt_callback` 检测停止标志立即返回；
2. **join**：按拉流 → 解码 → 推理 → 拼接 → 推流的顺序 join（推流线程退出时会发 RTMP trailer、RTSP TEARDOWN、GB28181 BYE 和注销）；
3. **释放共享资源**：最后才销毁 NPU 上下文，先销毁 dup 出来的实例，最后销毁 master。

另外 `main` 里忽略 SIGPIPE、SIGINT/SIGTERM 只设一个原子标志，`set_terminate` 在万一 terminate 时先打日志。

**追问：** 为什么不 detach？
→ detach 后线程什么时候结束不可控，进程退出时它可能还在访问已经析构的对象；而且没法保证"NPU 上下文在所有使用者结束之后才销毁"。

**代码：** `include/common/WorkerThread.hpp`、`src/app/App.cc` 的 `stop()`、`src/app/main.cc`。

#### A8 初始化顺序是怎么考虑的？
**答：** 两条原则：**先重资源、后外部连接**；**先消费者、后生产者**。
- 先加载模型：它最耗时、最容易失败（路径错、驱动版本不匹配、内存不够），失败时进程里还没有任何线程和网络连接，直接退出很干净；
- 再创建所有队列和缓冲；
- 启动顺序：推流 → 拼接 → 推理 → 解码 → 拉流，保证第一帧到来时下游都已就绪。

反过来的话，模型加载失败时已经有一堆线程和连接要清理，很容易出问题。

#### A9 `cv::Mat` 在多个线程之间传递怎么保证安全？
**答：** `cv::Mat` 是引用计数的，赋值是浅拷贝，计数本身是原子的，但像素数据没有保护。我的约定是"**发布后不再修改**"：写入方每帧都新建一个 Mat 写进 Mbuffer，之后不再改它；只读的消费者（拼接线程）直接用；需要修改的消费者（推流线程要叠加时间戳）先 `clone()` 再改。推理线程在帧上画框是安全的，因为解码后的缓冲只有它一个读者。

#### A10 锁的粒度怎么控制？
**答：** 锁内只做指针/引用计数级别的操作：入队出队、Mat 浅拷贝、更新序号。颜色转换、编码、网络 IO 全部在锁外。统计计数用 `std::atomic` 的 relaxed 累加，热路径完全无锁。

**追问：** GB28181 的 `write()` 持锁发 TCP 会不会阻塞 SIP 线程？
→ 会，最长到 socket 发送超时 2 秒。这是已知的取舍（见 Part 6），改进方法是发送放到独立队列/线程，锁里只取会话信息的副本。

#### A11 为什么用条件变量，不用 sleep 轮询？
**答：** 轮询要么浪费 CPU（间隔短），要么增加延迟（间隔长）。条件变量在数据到来时立即唤醒，不来时不占 CPU。所有等待都带谓词（防虚假唤醒）和超时（防退出卡死）。

#### A12 目标帧率控制怎么实现？
**答：** `FpsController` 有两种用法：
- 推理线程用"丢帧式" `accept()`：到了下一个时间点才接收这一帧，否则丢弃；
- 拼接/推流线程用"节拍式" `wait()`：睡到下一个时间点再产出。

关键是用**绝对时间点**累加周期（`next += period`），而不是每次 `sleep(period)`，这样处理耗时和 sleep 误差不会累积成帧率漂移；落后超过一个周期就重新对齐，不"追帧"；`accept` 允许 20% 的抖动，避免输入帧率刚好等于目标帧率时误丢。

---

### B 音视频与 Rockchip 硬件

#### B1 I 帧、P 帧、B 帧、GOP、IDR、SPS/PPS 分别是什么？
**答：**
- I 帧：帧内编码，自己就能解码；P 帧参考前面的帧；B 帧参考前后的帧（压缩率最高，但要重排序，增加延迟）；
- GOP：两个 I 帧之间的一组画面；
- IDR：特殊的 I 帧，解码器遇到它会清空参考帧列表，之后的帧不会参考它之前的帧，所以是"安全的起点"；
- SPS/PPS：序列参数集/图像参数集，描述分辨率、profile、熵编码方式等，解码器必须先拿到它们才能解码。

**追问：** 推流为什么不用 B 帧？
→ B 帧要等后面的帧编码完才能输出，增加延迟，而且 dts ≠ pts，处理更复杂；实时推流用 I/P 就够了。

#### B2 为什么丢包要丢到下一个关键帧？
**答：** P 帧依赖前面的帧。中间丢一个包，后面整个 GOP 都会因为参考帧缺失而花屏，直到下一个 IDR 才恢复。所以实时流队列满时，我清空积压的包，然后丢弃后面所有非关键帧，直到下一个关键帧才重新开始喂解码器——既不会花屏，也把延迟"截断"了。

#### B3 Annex-B 和 AVCC 的区别？为什么要转？
**答：** 都是 H.264 码流的组织方式。Annex-B 用起始码 `00 00 00 01` 分隔 NAL，SPS/PPS 在码流里（关键帧前）；AVCC 用 4 字节长度前缀，SPS/PPS 放在容器的 extradata（avcC box）里。MP4/FLV 用 AVCC，RTSP/TS 和 MPP 要 Annex-B，所以拉流后用 FFmpeg 的 `h264_mp4toannexb` 转换，它也会在关键帧前插入 SPS/PPS。对本来就是 Annex-B 的流，这个过滤器原样放行。

#### B4 NV12 是什么格式？为什么视频用 YUV？
**答：** NV12 是 YUV420 半平面格式：先是完整的 Y（亮度）平面，然后是 U、V 交错的一个平面，每 2×2 个像素共享一对 UV，所以大小是 宽×高×1.5。人眼对亮度敏感、对色度不敏感，降低色度采样率能省一半数据量而几乎看不出差别，所以视频编解码都用 YUV420。

#### B5 stride 是什么？为什么会有 stride？
**答：** stride 是一行像素在内存里实际占用的字节数（或像素数），可能比宽度大。硬件按 16/64 对齐访问内存效率高，所以 MPP 输出的帧宽高都会对齐，例如 1920×1080 的帧，`hor_stride = 1920`、`ver_stride = 1088`。

**坑：** UV 平面的起点是 `hor_stride × ver_stride`，不是 `宽 × 高`。按宽高算会从 Y 的填充行开始读色度，画面颜色错位发绿。我的 CPU 路径逐行拷贝、去掉 stride 填充后再 `cvtColor`。

**代码：** `src/decode/DecodeWorker.cc` 的 `onFrame`。

#### B6 什么是零拷贝？RGA 为什么用 fd？
**答：** MPP 解码出来的帧放在 DMA-BUF（DRM 分配的物理连续或 IOMMU 映射的内存）里，每块有一个文件描述符 fd。把 fd 直接交给 RGA，RGA 就能用 DMA 直接读硬件内存做颜色转换，CPU 完全不搬运像素，这就是零拷贝。

如果改用虚拟地址，RGA 要额外做页表映射；而且 RGA2 用虚拟地址时访问不了 4G 以上的内存（8G/16G 内存的板子会出问题）。用 fd 可以避开这些问题。

#### B7 MPP 解码流程？info_change 是什么？
**答：** `mpp_create` → `mpp_init` → 配置 `split_parse` → 循环：`decode_put_packet` 送包，`decode_get_frame` 取帧。

首帧（或分辨率变化时）解码器先吐出一个 **info_change** 帧，告诉你需要多大的 buffer。这时我创建一个 DRM buffer group，交给解码器（`MPP_DEC_SET_EXT_BUF_GROUP`），用 `mpp_buffer_group_limit_config` 限制最多 24 块，再 `MPP_DEC_SET_INFO_CHANGE_READY` 通知它继续。

**追问：** 为什么要限制 buffer 数量？
→ 不限制的话，下游卡住时解码器会一直申请新 buffer，内存无限增长。限制之后 buffer 用完解码器会暂停，形成天然背压。

**追问：** 解码出来的帧用完要做什么？
→ 马上 `mpp_frame_deinit` 归还。我在回调里同步完成颜色转换，转换完立刻归还；如果拿着不放，buffer group 被占满，解码器就卡死了。

#### B8 `split_parse` 是什么？
**答：** 告诉 MPP 输入是不是完整的一帧。设为 1 时 MPP 自己找帧边界，任何切分方式的码流都能喂（比如直接读 .h264 文件），代价是要看到下一帧开头才知道这一帧结束，多一帧延迟；设为 0 时调用方保证每次送一整帧，FFmpeg 读出来的就是整帧，延迟更低。默认 1 求稳妥，可配置。

#### B9 硬件出问题怎么办？
**答：** 每个硬件环节都有软件兜底，失败时自动回退并只告警一次：MPP 解码失败回退 FFmpeg 软解；RGA 失败回退 OpenCV；MPP 编码失败回退 FFmpeg 编码（依次试 `h264_rkmpp`、`libx264` 等）。同一套代码在 PC 上也能编译调试。

#### B10 MPP 编码怎么配置的？
**答：** H.264 High Profile、CBR 码率控制、可配置 GOP 和帧率；`MPP_ENC_SET_HEADER_MODE = EACH_IDR`，让每个 IDR 前都带 SPS/PPS；`MPP_ENC_GET_HDR_SYNC` 取出 SPS/PPS 作为 FLV/RTSP 的 extradata。输入 BGR 用 RGA 转成 NV12 直接写进 MPP 的 DRM buffer。需要关键帧时调 `MPP_ENC_SET_IDR_FRAME`。

#### B11 RTSP 断流了怎么发现？
**答：** 两层：FFmpeg 的 socket 超时选项（FFmpeg 5+ 叫 `timeout`，4.x 叫 `stimeout`——4.x 的 `timeout` 表示监听模式，用错会变成等别人连进来）；再加上 `interrupt_callback`，每次阻塞调用前设截止时间，超时就让 `av_read_frame` 返回错误。检测到错误就关闭输入、指数退避重连（500ms 起，逐次翻倍，封顶可配）。

---

### C 推理与融合

#### C1 NPU 三个核怎么用起来的？
**答：** RK3588 的 NPU 有 3 个核，一个 rknn_context 同一时刻只能被一个线程使用。我给每个模型创建多个实例：第一个用 `rknn_init` 加载，其余用 `rknn_dup_context` **共享权重**（只多占运行时内存）；然后用 `rknn_set_core_mask` 把所有模型的实例**轮询绑定**到 core0/1/2 上，三个核负载均衡。也可以配置成某个大模型绑定 `0_1_2` 三核一起跑。

**追问：** 实例数怎么定？
→ 实例数 = 该模型的最大并发度。一般至少 3 个（每个核一个）；路数多时可以更多，多出来的实例在同一个核上排队，能减少 CPU 预处理/后处理造成的 NPU 空闲。

#### C2 rknn_context 线程安全吗？你怎么保证？
**答：** 不是，同一个上下文不能被多个线程同时 `rknn_run`。我用实例池：每个模型有一个空闲实例列表，推理前用 RAII 的 `Lease` 从列表里取一个（没有就在条件变量上等），推理完析构时自动归还。这样保证一个实例同一时刻只在一个线程里，而且异常路径也不会忘记归还。

#### C3 多模型怎么并行？
**答：** 推理线程把同一帧对 N 个模型各提交一个任务到线程池，拿到 N 个 `future`，然后逐个 `get()` 等待。因为不同模型绑在不同的核上，它们真正并行执行：两个模型、每个 12ms，串行要 24ms，并行大约 13ms（PC 上用模拟库验证过这个时间关系，板上以实测为准）。

**追问：** 为什么必须等全部完成才返回？
→ 任务引用了调用方栈上的图像和结果数组，提前返回会变成悬空引用。

**追问：** 单模型也走线程池吗？
→ 不走，直接在推理线程里执行，省一次线程切换；4 路推理线程本身就提供了并发。

#### C4 预处理做了什么？坐标怎么映射回原图？
**答：** BGR 转 RGB，等比缩放到模型输入尺寸（640×640），四周用灰色 114 填充（letterbox），和 YOLOv5 训练时一致，比直接拉伸精度好。RGA 用一次 `improcess` 同时完成缩放、颜色转换和写入指定区域。

映射回原图：`x原图 = (x模型 − 左填充) / 缩放比例`，y 同理，然后裁剪到原图范围。例如 1280×720 输入：缩放 0.5，得到 640×360，上下各填充 140 行。

#### C5 YOLOv5 后处理怎么做的？有什么优化？
**答：** 模型有 3 个输出头（步长 8/16/32），每个头的通道是 3 个 anchor ×（5 + 类别数）。对每个格子、每个 anchor：取目标置信度和最大类别置信度，两者相乘作为得分，超过阈值就按公式还原框：`bx = (x·2 − 0.5 + 列号) · stride`，`bw = (w·2)² · anchor宽`。

优化：输出是 int8 量化的，我先把阈值换算到量化域，**直接用 int8 比较**，绝大多数格子不需要做浮点反量化；类别数也从输出张量形状自动推导（通道数/3 − 5）。

#### C6 NMS 的原理？你修的那个 bug 是什么？
**答：** NMS：按得分从高到低排序，依次取出最高分的框保留，把和它 IoU 超过阈值的**同类**低分框删掉，重复直到处理完。

原代码的 bug：排序后 `order[i]` 存的是原始下标，但它用排序后的位置 `i` 去查类别数组 `classIds[i]`，下标空间混用了；内层循环也没检查被比较框 `m` 的类别。结果是会跨类别误删（车把人删掉），同类的重复框反而删不干净。修复后用 `classIds[order[i]]` 和 `classIds[order[j]]` 判断。

另一个问题：原代码用全局标签数组加"首次加载"静态标志，多个推理线程第一次同时调用时会竞争，而且每个模型析构时会释放全局标签。我改成每个模型实例持有自己的标签，后处理没有任何全局可变状态。

**代码：** `src/infer/postprocess.cc`（文件头注释列了所有修改）。

#### C7 多模型结果怎么融合？
**答：** 五步：
1. 把所有模型的框放在一起，按"得分 × 模型权重"降序排序；
2. **关联**：每个框贪心地并入 IoU 最大、且超过阈值的同类簇；同一个簇里每个模型最多贡献一个框（用位掩码记录），因为同一模型的两个框在模型内部已经 NMS 过，必然是两个不同目标；
3. **簇内融合**：`weighted` 是 WBF 加权框融合，坐标按得分加权平均，得分按"检出的模型数/总模型数"折算；`confidence` 取最高分框，得分用 `1 − Π(1 − sᵢ)`，多模型一致时得分升高；`nms` 只保留最高分框；
4. **投票过滤**：簇里的框数少于 `min_votes` 的丢掉；
5. 融合结果再做一次 NMS，去掉相邻簇之间残留的重叠框。

**追问：** 为什么不直接把所有框合起来做一次 NMS？
→ 那样只保留了最高分的框，丢掉了"多个模型都认同"这个信息，没法通过投票降误检，也没法融合框的位置。

#### C8 误检和漏检怎么权衡？
**答：** 主要旋钮是 `min_votes`：设为 1 取并集，召回高、误检多；设为模型总数取交集，精度高、漏检多。再配合每个模型的置信度阈值、融合后的得分阈值和模型权重调整。实际项目里通常按场景定：安防告警宁可误报（低 min_votes），自动触发动作的场景要求精度（高 min_votes）。

#### C9 跳帧推理有什么副作用？
**答：** 复用的是之前帧的检测结果，目标快速运动时框会有滞后。所以复用结果有时效（`reuse_max_ms`，默认 500ms），过期就不画了；另外 `infer_interval` 要根据目标运动速度选，一般 2~3。更好的做法是在中间帧用跟踪算法（如卡尔曼滤波或光流）预测框的位置，这是改进方向。

---

### D 推流与 GB28181

#### D1 RTSP 的交互流程？RTP over TCP 和 UDP 有什么区别？
**答：** 拉流：`OPTIONS` → `DESCRIBE`（返回 SDP）→ `SETUP`（协商传输方式和端口）→ `PLAY` → 收 RTP，结束 `TEARDOWN`。推流是 `ANNOUNCE`（发 SDP）→ `SETUP` → `RECORD`。

UDP：延迟低，但可能丢包、乱序，还可能被防火墙/NAT 挡住；TCP（RTP 包交织在 RTSP 连接里传输）：可靠、穿透性好，但丢包重传会造成队头阻塞，网络差时延迟抖动大。项目默认用 TCP，可配置。

#### D2 RTMP 和 RTSP 有什么区别？
**答：** RTMP 基于 TCP，数据用 FLV 格式封装，常用于推流到 CDN/直播服务器，延迟一般 1~3 秒；RTSP 是控制协议，媒体走 RTP，常用于监控摄像头，延迟可以到几百毫秒。FLV 要求 SPS/PPS 作为 extradata（AVC sequence header）先发，RTSP 把它们放在 SDP 的 `sprop-parameter-sets` 里。

#### D3 为什么要等到关键帧再建立推流连接？
**答：** 两个原因：一是 FLV 和 RTSP 建连时需要 SPS/PPS 作为 extradata，要从编码器拿到；二是对端收到的第一帧必须是关键帧才能解码，否则会黑屏或花屏直到下一个 IDR。所以断开状态下遇到非关键帧就丢掉并请求编码器立刻出 IDR，拿到关键帧再建连。

另外每个关键帧都检查有没有带 SPS/PPS，没带就把缓存的补到前面，保证中途接入的观众和 GB28181 的 PS 流都能解码。

#### D4 SIGPIPE 是什么？推流端为什么要处理它？
**答：** 往一个对端已经关闭的 TCP 连接写数据时，内核会给进程发 SIGPIPE，默认动作是**直接终止进程**。推流服务器重启、网络断开时就会触发，表现为程序"无声无息地退出"，没有任何日志。处理方法：`signal(SIGPIPE, SIG_IGN)` 忽略它，这样 write/send 会返回 `EPIPE` 错误，由程序自己处理重连；或者 `send` 时带 `MSG_NOSIGNAL`。项目两者都用了。

**验证：** 用 mediamtx 做推流服务器，推流中途杀掉服务器，日志里是 `Broken pipe` → 退避重连 → 服务器恢复后重新推流成功，进程没有退出。

#### D5 GB28181 设备注册流程？
**答：**
1. 设备发 `REGISTER`（不带认证）；
2. 平台回 `401 Unauthorized`，带 `WWW-Authenticate`（realm、nonce）；
3. 设备计算 Digest：`HA1 = MD5(设备ID:realm:密码)`，`HA2 = MD5(REGISTER:uri)`，`response = MD5(HA1:nonce:HA2)`（有 qop 时还要加 nc、cnonce），带 `Authorization` 头重新 `REGISTER`；
4. 平台回 `200 OK`，注册成功。

之后在有效期的 80% 左右刷新注册；每隔 keepalive 秒发一次心跳 `MESSAGE`（Keepalive）；连续几次心跳没有回应，就判定平台掉线，用新的 Call-ID 重新注册。平台查询目录（Catalog）、设备信息时，先回 200，再发一条 `MESSAGE` 把结果报上去。

#### D6 GB28181 点播流程？SSRC 从哪来？
**答：** 平台发 `INVITE`，带 SDP：媒体接收 IP（`c=`）、端口和传输方式（`m=video 端口 RTP/AVP` 或 `TCP/RTP/AVP`）、SSRC（`y=`）。设备先回 `100 Trying`，再回 `200 OK` 带自己的 SDP（`sendonly`、`rtpmap:96 PS/90000`、同一个 `y=`），平台回 `ACK` 后设备开始发 RTP；平台发 `BYE` 就停止。

SSRC 必须用平台 SDP 里 `y=` 给的值，平台靠它区分是哪一路流，填错了平台收到数据也不认。

#### D7 PS 封装是什么结构？
**答：** GB28181 要求 H.264 先封装成 MPEG-2 PS（节目流）再用 RTP 发。每一帧：
- PS 包头（`00 00 01 BA`，14 字节，含 SCR 时钟）——每帧都有；
- 系统头（`00 00 01 BB`）和节目流映射 PSM（`00 00 01 BC`，声明流类型 0x1B = H.264，带 CRC32）——只在关键帧前；
- PES 包（`00 00 01 E0`）：装 H.264 数据，第一个 PES 带 PTS（33 位、90kHz）；PES 长度字段最大 65535，帧太大就拆成多个 PES。

**追问：** 33 位的 PTS 怎么放进 5 个字节？→ 见 Part 4 的位操作题，按"3 位 + marker、15 位 + marker、15 位 + marker"拆开。

#### D8 RTP 怎么分包？为什么是 1400 字节？
**答：** RTP 头 12 字节：版本 2、负载类型 96、序号（每包 +1）、时间戳（pts 毫秒 × 90）、SSRC。PS 数据按每包不超过 1400 字节切片，一帧的最后一片把 marker 位置 1，接收端据此知道一帧结束。

1400 是为了加上 IP/UDP/RTP 头后不超过以太网 MTU 1500，避免 IP 分片——分片中任何一片丢失整个包都作废。

TCP 模式按 RFC 4571，每个 RTP 包前面加 2 字节长度，解决 TCP 字节流没有边界（粘包）的问题。

#### D9 重复会话怎么处理？
**答：** 分三种情况：
1. **平台重传同一个 INVITE**（同一个 Call-ID，因为没收到我的 200 OK）：原样重发缓存的 200 OK，不建立第二路媒体流；
2. **推流中又来一个新的点播**（不同 Call-ID）：按配置，`replace` 先给旧会话发 BYE 再接受新的，`reject` 回 `486 Busy Here`；
3. **RTMP/RTSP 推到一个已经有人在推的路径**：服务器会拒绝（mediamtx 返回 400），日志明确提示路径可能被占用，然后退避重试。自己重连前总是先正常关闭旧连接（写 trailer / 发 TEARDOWN），避免自己在服务器上残留"僵尸会话"。配置加载时也会拒绝两个推流项推到同一个地址。

#### D10 端口被占用怎么办？
**答：** SIP 本地端口被占用（`bind` 返回 `EADDRINUSE`）时自动往后试，最多 20 个，并在 Via/Contact 里用实际端口；媒体端口被占用就回退到系统分配的端口，写进应答 SDP。

**追问：** 为什么 SIP 的 UDP socket 不设 `SO_REUSEADDR`？
→ Linux 上 UDP 设了 `SO_REUSEADDR` 后，多个进程可以绑定同一个端口，这时冲突不会报错而是"静默"发生（报文只被其中一个 socket 收到）。不设它，端口被占用时 `bind` 会明确失败，才能换端口。TCP 的媒体监听 socket 则设了它，为了能快速复用处于 TIME_WAIT 的端口。

#### D11 为什么重连要用指数退避？
**答：** 固定间隔重连，在服务器长时间不可用时会持续产生大量无效连接，浪费资源；如果很多设备同时断线，还会在服务器恢复瞬间一起涌上去（惊群）。指数退避（500ms、1s、2s……封顶可配）既能在短暂抖动时快速恢复，又能在长时间故障时降低压力。连上后退避时间重置。

---

### E 稳定性与调试

#### E1 内存泄漏怎么排查？
**答：** 分四步：
1. **先确认是不是真泄漏**：长时间观察 `/proc/<pid>/status` 的 VmRSS 趋势，至少覆盖几次重连和文件循环。涨到一个平台后稳定的，是分配器缓存、碎片或缓冲区填满，不是泄漏；线性持续增长才是泄漏；
2. **分类**：同时看 fd 数（句柄泄漏）、线程数（线程泄漏）、CMA/dma_buf（硬件内存）、普通堆；
3. **普通堆**：用 ASan/LeakSanitizer 编译运行，正常退出时它会打印泄漏的分配栈；长时间运行用 heaptrack；PC 上可以用 valgrind；
4. **缩小范围**：只改配置逐个关模块（关推流、关显示、关 RGA、解码改软解、只留一路），看泄漏在哪个模块消失。

项目里在 PC 上用 ASan 跑过 4 路 + 2 模型 + 2 路推流，没有报告泄漏。

#### E2 MPP/NPU 这些硬件内存泄漏，ASan 能查到吗？
**答：** 查不到。MPP 的解码/编码 buffer、NPU 的权重和中间结果都不走 malloc，堆工具看不到。只能：
- 看系统计数：`/proc/meminfo` 的 CmaFree、`/sys/kernel/debug/dma_buf/bufinfo` 的对象数和总字节数是否持续增长；
- 看库自己的调试日志：MPP 的 `mpp_buffer_debug` 环境变量、librga 的 `ROCKCHIP_RGA_LOG`；
- **代码审查"申请/释放"对照表**：每个 `mpp_frame` 都要 `mpp_frame_deinit`，每次 `rknn_outputs_get` 都要 `rknn_outputs_release`（漏掉就是每帧约 2MB 的泄漏），`mpp_buffer_group_put` 要在 `mpp_destroy` 之后……（完整表见学习指南 12.4.5）。

#### E3 程序卡死了怎么查？
**答：** `gdb -p <pid>` 然后 `thread apply all bt`，看每个线程卡在哪：卡在 `pthread_cond_wait` 就看它在等哪个条件、谁应该唤醒它；卡在 `__lll_lock_wait` 就查两个线程的加锁顺序；卡在 `poll`/`recv`/`connect` 就查网络调用有没有超时。退出时卡住，最常见的原因是某个队列没有 `close()`，消费者还在 `pop()` 上等。

#### E4 程序崩溃了怎么查？
**答：** 用 `RelWithDebInfo` 编译保留符号，`ulimit -c unlimited` 并设置 `core_pattern`，崩溃后 `gdb 程序 core` 看 `bt`。越界、释放后使用这类问题用 ASan 复现最快。如果进程没有任何栈就消失了，先怀疑 SIGPIPE 和 OOM killer（`dmesg | grep -i oom`）。

#### E5 性能瓶颈怎么定位？
**答：** 先用 `performance.sh` 把 CPU/NPU 定频，减少测试误差。然后看项目内置的性能报告：每个阶段的调用频率、平均耗时、最大耗时和负载（= 频率 × 平均耗时，也就是占用一个线程的比例），负载超过 85% 会被标为瓶颈；再看丢帧计数：`drop.pkt` 增长是解码跟不上，`drop.frame` 增长是推理跟不上，`<模型>.wait` 大说明实例不够在排队。配合 `top -H` 看各线程 CPU、`/sys/kernel/debug/rknpu/load` 看三个 NPU 核的占用、`perf top` 看 CPU 热点函数。

#### E6 用过 TSan 吗？报告都是真的吗？
**答：** 用过。项目在 PC 上跑 TSan 报了 5 条，分析后全是误报：FFmpeg 的帧线程和 OpenCV `cv::Mat` 的引用计数，是在没有插桩的动态库里用原子指令同步的，TSan 看不到这种同步，就把"引用计数归零后的 free"报成了数据竞争。判断方法是看报告的两个栈：如果两边都落在第三方库的引用计数/释放逻辑里，访问的也不是我们自己的成员变量，大概率是误报，可以用 suppressions 文件屏蔽。

#### E7 怎么保证长时间运行的稳定性？
**答：** 设计上：所有队列有界、Mbuffer 只存一帧、硬件 buffer 限制数量，内存不会无限增长；所有网络操作带超时、断线退避重连；线程生命周期统一管理。验证上：ASan/TSan 检查；人为制造异常（杀掉推流服务器、断开摄像头、平台重启）看能否自动恢复；长时间跑（24 小时以上）记录 RSS、fd 数、线程数曲线（上板后要实测，见 Part 7）。

#### E8 日志是怎么设计的？
**答：** 线程安全（一把锁保护一次完整输出），格式是"时间.毫秒 [级别] [线程名] 内容"，警告和错误走 stderr；级别可配置，debug 级别会打印每一条 SIP 收发报文。所有线程都有名字，看日志能直接知道是哪一路、哪个模块。高频路径不打日志，用计数器统计，由 perf 线程定期汇总。

---

### F 扩展与设计权衡

#### F1 如果要支持 16 路、32 路，怎么改？
**答：** 先算硬件上限：RK3588 的 VPU 官方宣称约 32 路 1080p30 解码，NPU 6 TOPS，yolov5s 单次十几毫秒——路数多了 NPU 会先到瓶颈。改法：
1. **降推理负载**：调低每路推理帧率、加大跳帧间隔、用更小的模型或输入尺寸；
2. **线程模型**：每路 3 个线程不适合几十路，改成固定数量的工作线程 + 事件驱动（epoll）处理拉流，解码/推理用任务队列；
3. **减少内存拷贝**：NPU 输入零拷贝（`rknn_create_mem` + `rknn_set_io_mem`，RGA 直接写进 NPU 输入内存），帧缓冲池化，避免每帧分配 6MB；
4. **子码流**：摄像头的子码流（如 640×360）专门用来推理，主码流只做录像/转发。

#### F2 为什么不用 GStreamer 这类现成框架？
**答：** GStreamer 在 Rockchip 上有 MPP、RGA 插件，搭一条标准管线很快。但这个项目需要多模型并发、结果融合、自定义丢帧策略、GB28181，这些都要写自定义插件，调试成本不低。自己用 C++ 写，每一环的线程、缓冲和丢弃策略都完全可控，也更能体现对底层的理解。实际工作中如果需求是标准管线，我会优先考虑 GStreamer 或 FFmpeg-rockchip。

#### F3 为什么每路一个线程，而不是 epoll？
**答：** 4 路的规模下，每路独立线程最简单，逻辑直观，一路卡住不影响其它路，而且 FFmpeg 的 `av_read_frame` 本身是阻塞接口，不太适合放进 epoll。路数多了再改成事件驱动（见 F1）。

#### F4 为什么自己实现 GB28181，不用 ZLMediaKit？
**答：** ZLMediaKit 功能完整，生产环境我会优先考虑。自己实现一方面是想把 SIP 信令、Digest 认证、PS 封装、RTP 分包这些协议细节真正弄懂；另一方面设备端只需要注册、心跳、目录、点播这几个功能，代码量可控，没有额外依赖。目前的局限是只支持一路会话、SIP 只走 UDP、只做了 H.264（见 Part 6）。

#### F5 端到端延迟怎么测？怎么降低？
**答：** 测：最直接的是"玻璃到玻璃"——摄像头对着一个毫秒计时器，把计时器和显示/推流画面拍在同一张照片里，读两个时间差；推流画面上也叠加了本地时间，可以和播放端时间对比。代码层面可以在解码时打时间戳、一路带到输出，统计每一段的耗时（目前 Mbuffer 在每一级写入时都会刷新时间戳，还没有贯穿全程的时间戳，是可以补的点）。

降：`split_parse=0` 省一帧；播放端/服务器缓冲调小；不用 B 帧；GOP 不要太长；推理跳帧复用结果；RTSP 拉流可以用 UDP。

#### F6 NPU 算力不够怎么办？
**答：** 按代价从小到大：降低推理帧率（`target_fps`/`infer_interval`）→ 降低输入分辨率 → 换更小的模型或更激进的量化 → 多路图像拼成一张大图一次推理（batch/拼图推理）→ 用运动检测等轻量方法先过滤掉静止画面，只对有变化的画面推理。


---

## Part 3 Linux / C++ 基础八股（结合项目）

> 每题先给标准答案，再给 **🔗 结合项目**：面试时用项目里的例子回答，比背书有说服力得多。

**本部分目录：**
1. [多线程与同步](#1-多线程与同步)（必考）
2. [进程与进程间通信](#2-进程与进程间通信)
3. [网络编程](#3-网络编程)（必考）
4. [内存管理](#4-内存管理)
5. [信号](#5-信号)
6. [C++ 语言](#6-c-语言)
7. [编译、链接与工具](#7-编译链接与工具)
8. [部署与运维](#8-部署与运维)

---

### 1. 多线程与同步

#### 1.1 互斥锁、条件变量、信号量、自旋锁、读写锁的区别？
- **互斥锁**：同一时刻只允许一个线程进入临界区，拿不到就睡眠。
- **条件变量**：配合互斥锁使用，让线程"等某个条件成立"，条件变化时由其他线程通知唤醒。
- **信号量**：带计数的同步原语，可以允许 N 个线程同时进入，也常用于"资源计数"。
- **自旋锁**：拿不到锁时忙等不睡眠，适合临界区极短、不想付出线程切换代价的场景（内核里常用，用户态慎用）。
- **读写锁**：读和读可以并发，写独占，适合读多写少。

🔗 **结合项目**：队列和 Mbuffer 用 `mutex + condition_variable`；模型实例池本质上是一个"计数信号量"（空闲实例数），我用 `mutex + condition_variable + 空闲列表` 实现，这样还能拿到具体是哪个实例。

#### 1.2 条件变量为什么要配合 while（或谓词）使用？
- **虚假唤醒**：线程可能在没有被 notify 的情况下醒来（系统实现允许）。
- **被抢先**：被唤醒到重新拿到锁之间，条件可能已被别的线程改掉了（比如数据已被另一个消费者取走）。

所以醒来后必须重新检查条件：`while (!cond) cv.wait(lock);`，或者用带谓词的 `cv.wait(lock, pred)`，两者等价。

🔗 **结合项目**：所有等待都是 `wait_for(lock, 超时, 谓词)` 形式，谓词里除了数据条件还检查 `closed_`，保证退出时能被唤醒（`include/common/BlockingQueue.hpp`）。

#### 1.3 notify_one 和 notify_all 的区别？什么时候用哪个？
- `notify_one` 唤醒一个等待者，`notify_all` 唤醒全部。
- 每次只多出一个"资源"（入队一个元素）时用 `notify_one`，避免惊群；状态发生全局变化（关闭、停止）时必须 `notify_all`，否则可能有线程永远醒不过来。
- 小细节：notify 可以在解锁之后调用，减少被唤醒的线程马上又阻塞在锁上的情况。

🔗 **结合项目**：队列入队/出队用 `notify_one`，`close()` 用 `notify_all`；Mbuffer 写入用 `notify_all`（可能有多个读者）。

#### 1.4 死锁的四个必要条件？怎么预防？
- 互斥、占有并等待、不可剥夺、循环等待。
- 预防：固定加锁顺序（破坏循环等待）；一次性申请所有资源或持有时不再申请（破坏占有并等待）；`try_lock` 失败就释放已持有的锁（破坏不可剥夺）；用 `std::lock`/`std::scoped_lock` 同时锁多个互斥量；锁内不调用外部回调。

🔗 **结合项目**：线程池线程数 = 模型实例数，每个任务只持有一个实例且持有期间不再等别的资源，破坏"占有并等待"（Part 2 A6）。

#### 1.5 `std::atomic` 和 `volatile` 的区别？memory_order 了解吗？
- `volatile` 只保证每次都从内存读、不被编译器优化掉，**不保证原子性，也不保证多线程可见顺序**，不能用于线程同步（它是给硬件寄存器、信号处理用的）。
- `std::atomic` 保证操作原子，并通过 memory_order 控制可见顺序：
  - `relaxed`：只保证原子，不保证顺序，适合计数器；
  - `acquire/release`：release 之前的写，对 acquire 到同一变量的线程可见，适合"发布数据 + 标志位"；
  - `seq_cst`（默认）：全局一致顺序，最安全最慢。

🔗 **结合项目**：性能统计计数用 `fetch_add(relaxed)`（只要最终数对，不要求顺序）；停止标志用默认的 `seq_cst`；手写 shared_ptr 引用计数减到 0 时用 `acq_rel`（见 Part 4 题 4）。

#### 1.6 线程池的原理？线程数怎么定？
- 预先创建一组线程，从任务队列取任务执行，避免频繁创建销毁线程的开销，并限制并发度。
- 线程数：CPU 密集型 ≈ 核数；IO 密集型可以更多（核数 × (1 + 等待时间/计算时间)）；**受外部资源限制时，按资源数定**。

🔗 **结合项目**：推理线程池的线程数 = 模型实例数，因为真正的瓶颈是 NPU 实例，多出来的线程只会排队等实例。`future` 用来拿结果和传递异常。

#### 1.7 线程和协程的区别？
- 线程由内核调度，切换要进内核，每个线程有独立的内核栈和较大的用户栈（默认 8MB 虚拟空间）；协程在用户态由程序自己调度，切换只是保存/恢复几个寄存器，非常轻量，但不能利用多核（除非配合多线程），遇到阻塞系统调用会卡住整个线程。

---

### 2. 进程与进程间通信

#### 2.1 进程和线程的区别？
- 进程是资源分配单位（独立地址空间、文件描述符表），线程是调度单位（共享进程的地址空间和资源，各自有栈和寄存器）。
- 线程间通信方便（共享内存），但一个线程崩溃整个进程都崩；进程隔离好，但通信要靠 IPC，创建和切换开销更大。

🔗 **结合项目**：用多线程，因为各级之间要传每帧几 MB 的图像，共享内存零拷贝最方便。如果要求"推流模块崩了不影响检测"，可以把推流拆成独立进程，通过共享内存 + 信号量传帧。

#### 2.2 进程间通信有哪些方式？
- 管道（匿名管道，父子进程）、命名管道 FIFO、消息队列、**共享内存**（最快，要配合信号量/互斥锁同步）、信号量、信号、**socket**（包括 Unix 域 socket，可以跨机器）、内存映射文件、DMA-BUF fd 传递（音视频/图形常用）。

#### 2.3 僵尸进程和孤儿进程？
- 僵尸进程：子进程退出了，父进程没有 `wait`/`waitpid` 回收，进程表项还留着。解决：父进程回收，或者处理 `SIGCHLD`，或者设置 `SIGCHLD` 为 `SIG_IGN`。
- 孤儿进程：父进程先退出，子进程被 init/systemd 收养，无害。

#### 2.4 fork 之后多线程程序有什么问题？
- `fork` 只复制调用它的那个线程；如果当时别的线程持有某把锁（比如 malloc 内部的锁），子进程里这把锁永远不会被释放，子进程一调用 malloc 就可能死锁。所以多线程程序里 fork 之后应该尽快 `exec`。

---

### 3. 网络编程

#### 3.1 select / poll / epoll 的区别？
| | select | poll | epoll |
|---|---|---|---|
| fd 数量上限 | FD_SETSIZE（通常 1024） | 无 | 无 |
| 每次调用 | 要把整个 fd 集合从用户态拷贝到内核 | 同左 | 通过 `epoll_ctl` 注册一次，之后不用再拷贝 |
| 就绪检查 | 内核和用户都要遍历全部 fd，O(n) | O(n) | 内核用回调把就绪 fd 放进就绪链表，`epoll_wait` 只返回就绪的，O(就绪数) |
| 触发方式 | 水平触发 | 水平触发 | 水平触发(LT) / 边缘触发(ET) |

- **LT**：只要还有数据没读完，每次 `epoll_wait` 都会通知；**ET**：只在状态变化时通知一次，必须用非阻塞 fd，并循环读到 `EAGAIN`，否则剩下的数据不会再通知。

🔗 **结合项目**：GB28181 的 SIP 线程只监听 1 个 UDP socket，用 `poll` 带 100ms 超时，既能收报文又能定时处理注册刷新、心跳；fd 很少时 poll 和 epoll 没有区别。如果改成几十路拉流用事件驱动，就该用 epoll。

#### 3.2 TCP 三次握手、四次挥手？TIME_WAIT 是什么？
- 三次握手：SYN → SYN+ACK → ACK。为什么要三次：双方都要确认对方能收也能发，并同步初始序号；两次的话，已失效的旧连接请求可能被误建立。
- 四次挥手：FIN → ACK →（对方数据发完）FIN → ACK。因为 TCP 全双工，两个方向要分别关闭。
- TIME_WAIT：**主动关闭方**在最后一个 ACK 之后等待 2MSL，保证最后的 ACK 丢了还能重发，并让网络里这个连接的旧报文全部过期，不影响新连接。大量短连接的服务端会堆积 TIME_WAIT。

#### 3.3 SO_REUSEADDR 和 SO_REUSEPORT？
- `SO_REUSEADDR`（TCP）：允许绑定处于 TIME_WAIT 的端口，服务重启时不用等。
- 在 Linux 的 UDP 上，`SO_REUSEADDR` 允许多个 socket 绑定同一地址和端口，单播报文只会送给其中一个。
- `SO_REUSEPORT`：多个 socket 绑定同一端口，内核做负载均衡，常用于多进程/多线程服务器。

🔗 **结合项目**：GB28181 的 TCP 媒体监听 socket 设了 `SO_REUSEADDR`（快速复用端口）；SIP 的 UDP socket **故意不设**，这样端口被别的进程占用时 `bind` 会明确失败，我才能换下一个端口，而不是两个进程"静默"抢同一个端口（Part 2 D10）。

#### 3.4 非阻塞 connect 怎么实现超时？
1. 把 socket 设为 `O_NONBLOCK`；
2. `connect` 返回 -1 且 `errno == EINPROGRESS` 表示正在连接；
3. 用 `poll`/`select` 等待可写（`POLLOUT`），设置超时；
4. 可写后用 `getsockopt(SO_ERROR)` 取结果，0 表示成功；
5. 需要的话再把 socket 改回阻塞，并设 `SO_SNDTIMEO` 发送超时。

🔗 **结合项目**：GB28181 TCP 主动模式连接平台就是这么写的，超时 3 秒（`Gb28181Sink::connectMedia`）。

#### 3.5 TCP 粘包是什么？怎么解决？
- TCP 是字节流，没有消息边界：发送方两次 `send` 的数据，接收方可能一次收到，也可能分几次收到。
- 解决：定长消息；特殊分隔符（如 HTTP/SIP 头部的 `\r\n\r\n`）；**长度前缀**（最常用）。

🔗 **结合项目**：GB28181 的 RTP over TCP 用 RFC 4571，每个 RTP 包前加 2 字节长度；SIP 报文用 `\r\n\r\n` 分头部和正文，再用 `Content-Length` 确定正文长度。

#### 3.6 TCP 和 UDP 的区别？音视频为什么常用 UDP？
- TCP：面向连接、可靠、有序、有流量和拥塞控制，但丢包重传会造成队头阻塞，延迟抖动大。
- UDP：无连接、不可靠、保留消息边界、开销小、延迟低。
- 实时音视频"宁可丢一帧也不要卡住等重传"，所以常用 UDP（RTP），可靠性靠上层（FEC、NACK、关键帧请求）；需要穿透防火墙或网络很差时改用 TCP。

#### 3.7 `send` 返回值要注意什么？
- 阻塞 socket 也可能只发出一部分（被信号打断、发送缓冲区不足），要循环发送直到发完；返回 -1 要看 `errno`：`EINTR` 重试，`EAGAIN` 非阻塞时缓冲区满，`EPIPE` 对端关闭（同时会有 SIGPIPE）。

🔗 **结合项目**：`Gb28181Sink::sendAll` 就是循环发送 + `EINTR` 重试 + `MSG_NOSIGNAL`。

---

### 4. 内存管理

#### 4.1 虚拟内存是什么？为什么需要？
- 每个进程看到的是独立的虚拟地址空间，通过页表映射到物理内存（按页，一般 4KB）。好处：进程间隔离；程序可以使用比物理内存大的地址空间；按需分配（缺页时才真正分配物理页）；共享库、共享内存可以映射到多个进程。

#### 4.2 进程的内存布局？
- 从低到高：代码段（text）、数据段（已初始化全局变量）、BSS（未初始化全局变量）、堆（向上增长）、mmap 区（共享库、大块 malloc、文件映射）、栈（向下增长）、内核空间。

#### 4.3 malloc 是怎么工作的？
- glibc 的 malloc（ptmalloc）：小块内存从堆上分配（通过 `brk` 扩展堆），释放后放进空闲链表（bins）复用，不一定马上还给系统；大块内存（超过 `M_MMAP_THRESHOLD`，默认 128KB，会动态调整）直接用 `mmap` 分配，`free` 时 `munmap` 立即归还。
- 多线程时，每个线程可能用不同的分配区（arena），减少锁竞争，但会让内存"看起来"占用更多。

🔗 **结合项目**：每帧 1080p BGR 约 6MB，属于大块分配。排查"内存上涨"时要区分真泄漏和分配器缓存：可以用 `MALLOC_ARENA_MAX=2` 或调用 `malloc_trim(0)` 验证是不是 arena 缓存（学习指南 12.4.1）。

#### 4.4 RSS、VSZ、PSS 分别是什么？
- VSZ：虚拟内存大小，包括所有映射（很多没有真正分配物理页），参考意义小。
- RSS：实际驻留在物理内存中的大小，包括共享库，**看泄漏主要看它的趋势**。
- PSS：按共享比例分摊后的大小，多进程统计总内存时更准确。
- 查看：`/proc/<pid>/status`（VmRSS、VmHWM 峰值）、`/proc/<pid>/smaps`（每段映射的明细）、`pmap -x`。

#### 4.5 内存泄漏、内存越界、野指针分别怎么查？
- 泄漏：ASan/LeakSanitizer、valgrind memcheck、heaptrack。
- 越界、释放后使用、重复释放：ASan（最快）、valgrind。
- 预防：RAII、智能指针、容器代替裸数组、有界容器。

🔗 **结合项目**：见 Part 2 E1/E2，重点说"硬件内存 ASan 看不到，要看 CMA/dma_buf 和对照申请释放表"，这是嵌入式音视频特有的点。

#### 4.6 OOM killer 是什么？
- 系统内存耗尽时，内核按 oom_score 选一个进程杀掉。表现是进程突然消失、没有 core。排查：`dmesg | grep -i oom`。可以调 `/proc/<pid>/oom_score_adj` 保护关键进程。

---

### 5. 信号

#### 5.1 信号处理函数里能做什么？
- 只能调用**异步信号安全**的函数（`write`、`_exit`、`sem_post` 等，见 `man 7 signal-safety`）。`printf`、`malloc`、加锁都不安全：信号可能在主流程持有 malloc 锁时到来，处理函数里再 malloc 就死锁了。
- 最佳实践：处理函数里只设置一个 `volatile sig_atomic_t` 或无锁的 `std::atomic<bool>` 标志，主循环检查标志后正常退出。

🔗 **结合项目**：`main.cc` 的 SIGINT/SIGTERM 处理函数只做 `g_quit = true`，主循环看到后走正常的退出流程。

#### 5.2 `signal` 和 `sigaction` 的区别？
- `signal` 的语义在不同系统上不一致（处理后是否复位、被打断的系统调用是否自动重启）；`sigaction` 行为明确，可以设置信号屏蔽字和 `SA_RESTART` 等标志，推荐使用。

🔗 **结合项目**：SIGINT/SIGTERM 用 `sigaction`；SIGPIPE 只是忽略，用 `signal(SIGPIPE, SIG_IGN)` 足够。

#### 5.3 常见信号？
- SIGINT（Ctrl+C）、SIGTERM（kill 默认，可捕获，用于优雅退出）、SIGKILL（不可捕获）、SIGSEGV（非法内存访问）、SIGABRT（abort，`std::terminate` 最终会触发）、**SIGPIPE**（写已关闭的连接，默认终止进程）、SIGCHLD（子进程状态变化）。

---

### 6. C++ 语言

#### 6.1 RAII 是什么？
- 资源获取即初始化：在构造函数里获取资源，在析构函数里释放，靠对象生命周期自动管理，异常路径也不会泄漏。

🔗 **结合项目**：`WorkerThread`（析构 join）、`ModelManager::Lease`（析构归还实例）、`ScopedTimer`（析构记录耗时）、`std::unique_ptr` 管理各类对象。

#### 6.2 shared_ptr、unique_ptr、weak_ptr？shared_ptr 线程安全吗？
- `unique_ptr`：独占所有权，零开销，只能移动。
- `shared_ptr`：共享所有权，引用计数；`weak_ptr` 不增加计数，用来打破循环引用、观察对象是否还活着。
- 线程安全：**控制块的引用计数是原子的**，多个线程各自拷贝/销毁不同的 `shared_ptr` 实例是安全的；但**同一个 shared_ptr 对象**被多线程同时读写不安全；**指向的对象**本身也没有任何保护。

🔗 **结合项目**：`VideoPacket` 用 `shared_ptr` 在线程间传递；`AVCodecParameters` 用 `shared_ptr` + 自定义删除器（`avcodec_parameters_free`）管理 C 库资源；标签列表用 `shared_ptr<const vector<string>>` 在同一模型的多个实例间共享（const 保证只读，天然线程安全）。

#### 6.3 移动语义和完美转发？
- 移动语义：通过右值引用 `T&&` 和移动构造/赋值，把资源"搬走"而不是复制（比如 `std::vector` 移动只交换指针）。`std::move` 只是把左值转成右值引用，本身不移动任何东西。
- 完美转发：模板里用 `T&&`（万能引用）+ `std::forward<T>`，保持参数原本的左值/右值属性传给下一层。

🔗 **结合项目**：队列 `push(T value)` 内部 `std::move` 进容器；线程池 `submit` 用完美转发把可调用对象和参数传给 `std::bind`。

#### 6.4 lambda 捕获有什么坑？
- 按引用捕获 `[&]` 或捕获 `this`，如果 lambda 比被捕获的对象活得久（比如交给另一个线程异步执行），就会访问悬空引用。
- 按值捕获指针也只是拷贝指针，对象本身还是可能先被销毁。

🔗 **结合项目**：`inferAll` 里任务按引用捕获了调用方栈上的图像和结果数组，所以必须等所有 future 完成才能返回；各 Worker 的线程 lambda 捕获了 `this`，所以析构函数里必须先 join 线程。

#### 6.5 虚函数是怎么实现的？析构函数为什么要是虚的？
- 每个有虚函数的类有一张虚函数表，每个对象有一个虚表指针，调用虚函数时通过虚表间接调用，实现运行时多态。
- 通过基类指针删除派生类对象时，如果析构函数不是虚的，只会调用基类析构函数，派生类的资源泄漏（未定义行为）。

🔗 **结合项目**：`VideoDecoder`（MPP / FFmpeg）、`VideoEncoder`（MPP / FFmpeg）、`IStreamSink`（RTMP、RTSP / GB28181）都是抽象接口 + 虚析构，用工厂函数按配置和硬件可用性创建具体实现。

#### 6.6 单例模式怎么写线程安全？
- C++11 起，函数内的静态局部变量初始化是线程安全的（"magic statics"），最简单的写法是 `static T& instance() { static T inst; return inst; }`（见 Part 4 题 3）。双重检查锁在 C++11 之前容易写错（指令重排导致拿到未构造完成的对象）。

🔗 **结合项目**：`PerfMonitor::instance()` 就是这种写法。

#### 6.7 `std::thread` 析构、异常、detach 的规则？
- `std::thread` 析构时如果仍 joinable（没有 join 也没有 detach），调用 `std::terminate`。
- 线程函数抛出的异常如果没被捕获，也会 `std::terminate`；要把异常传回调用方，用 `std::packaged_task`/`std::async`，异常会存进 future，`get()` 时重新抛出。
- detach 后线程独立运行，主线程无法再等它，进程退出时它可能还在访问已析构的对象。

🔗 **结合项目**：Part 2 A7。

#### 6.8 `std::function` 和函数指针的区别？
- 函数指针只能指向普通函数（或无捕获的 lambda）；`std::function` 可以装任何可调用对象（带捕获的 lambda、仿函数、bind 结果），代价是可能有堆分配和一次间接调用。

🔗 **结合项目**：解码器回调用 C 风格函数指针 + `userdata`（和 MPP 示例风格一致，零开销）；线程入口、拼接线程的状态回调用 `std::function`。

---

### 7. 编译、链接与工具

#### 7.1 交叉编译要注意什么？
- 用目标平台的工具链（如 `aarch64-linux-gnu-g++`）；依赖库也要是目标架构的，通常通过 sysroot 提供头文件和库；CMake 用 toolchain 文件设置 `CMAKE_SYSTEM_NAME`、编译器和 `CMAKE_FIND_ROOT_PATH`，避免误找到主机上的库。

🔗 **结合项目**：`build-linux_RK3588.sh` 设置了 aarch64 编译器；RGA 和 RKNN 的 aarch64 库随仓库提供；PC 上调试时要关闭 RGA（否则会链接到 aarch64 版的 librga）。

#### 7.2 动态库是怎么被找到的？rpath、runpath、LD_LIBRARY_PATH 的顺序？
- 查找顺序：`DT_RPATH`（只有在没有 `DT_RUNPATH` 时才生效）→ `LD_LIBRARY_PATH` → `DT_RUNPATH` → `/etc/ld.so.cache` → 默认目录（`/lib`、`/usr/lib`）。
- 较新的链接器默认生成 RUNPATH，所以 `LD_LIBRARY_PATH` 优先级更高。
- 排查工具：`ldd` 看依赖解析到了哪里，`readelf -d` 看 RPATH/RUNPATH，`LD_DEBUG=libs` 看加载过程。

🔗 **结合项目**：CMake 设置 `$ORIGIN/lib`，安装目录整体拷贝到任何位置都能找到 `lib/` 下的 librknnrt 和 librga。

#### 7.3 静态库和动态库的区别？
- 静态库链接时拷进可执行文件，部署简单、无版本依赖问题，但体积大，库升级要重新链接；动态库运行时加载，多个进程共享一份物理内存，可以单独升级，但要处理版本兼容和查找路径。

#### 7.4 常用调试工具？
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

### 8. 部署与运维

#### 8.1 怎么让程序开机自启、崩溃后自动重启？
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

#### 8.2 看门狗？
- 软件看门狗：systemd 的 `WatchdogSec` + 程序定期调用 `sd_notify("WATCHDOG=1")`，程序卡死不喂狗就被重启。可以在主循环或 perf 线程里检查"各路是否还在出帧"，都正常才喂狗（本项目还没做，是改进点）。
- 硬件看门狗：`/dev/watchdog`，系统级卡死时重启整机。


---

## Part 4 手撕代码

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

### 题 1：有界阻塞队列 🔗

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

### 题 2：线程池 🔗

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
- 🔗 项目用的 `dpool::ThreadPool` 还支持按需创建线程、空闲 2 秒回收。线程数 = 模型实例数，保证不会死锁（Part 2 A6）。

---

### 题 3：两个线程交替打印 1~n

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

### 题 4：简易 shared_ptr

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
- "shared_ptr 线程安全吗"：计数安全，对象不安全，同一个 shared_ptr 实例被并发读写也不安全（Part 3 6.2）。

---

### 题 5：线程安全单例 🔗

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

### 题 6：最新帧缓冲（单槽覆盖）🔗

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

### 题 7：memmove（处理内存重叠）

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

### 题 8：LRU 缓存

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

### 题 9：IoU + NMS 🔗

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

**要能讲的点**：复杂度 O(n²)；🔗 项目里原来的 NMS bug 是"排序后的位置"和"原始下标"混用、没检查被抑制框的类别（Part 2 C6），手写时要注意同一个数组里下标含义要一致。

---

### 题 10：33 位 PTS 打包进 5 字节 🔗

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

### 题 11：大端写入 RTP 头 🔗

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


---

## Part 5 "你遇到过最难的问题是什么"——踩坑故事

> **诚信第一**：每个故事都标注了来源。
> - ✅ **真实发生**：在本项目开发/测试过程中确实遇到并解决的，可以直接讲；
> - 🛡️ **预防性设计**：代码里做了防护、并做过验证，但不是"线上事故"。讲的时候说"我在设计时就考虑到了……并做了测试验证"，**不要说成自己踩过的坑**，除非你上板后真的遇到了。
>
> 讲法用 STAR：**S** 背景 → **T** 任务 → **A** 行动（重点，讲定位过程）→ **R** 结果，最后准备好追问。建议熟练掌握 3 个：一个 C++/编译相关（故事 2）、一个算法/多线程相关（故事 1）、一个调试方法论相关（故事 3）。

---

### 故事 1：NMS 下标混用导致跨类别误删框 ✅

- **S**：项目最初是单路单线程的 YOLOv5 demo，后处理代码来自官方示例。我要把它改成多路多模型并发推理。
- **T**：改造时要保证后处理线程安全，同时核对后处理逻辑的正确性。
- **A**：
  1. 读 `nms()` 时发现，排序后的数组 `order[i]` 存的是候选框的**原始下标**，但判断类别用的是 `classIds[i]`——拿"排序后的位置"去查"按原始下标存的类别数组"，下标空间混用了；内层循环还用了 `classIds[i]` 而不是 `classIds[m]`，根本没检查被比较框的类别；
  2. 构造了一个例子验证：三个候选框 person 0.6、car 0.9、person 0.8，排序后 order = [1, 2, 0]，按原逻辑处理 person 类时会用错位置的类别判断，出现用车框去抑制人框、同类重复框漏删；
  3. 修复为 `n = order[i]; classIds[n]`、`m = order[j]; classIds[m]`；
  4. 同时发现另一个问题：标签是全局数组，加上一个静态"首次加载"标志，多个推理线程第一次同时调用会竞争，而且每个模型析构时都会 free 全局标签。改成每个模型持有自己的标签（`shared_ptr<const vector<string>>`），后处理完全无全局状态。
- **R**：多模型并发推理时不再有共享可变状态；NMS 按类别正确抑制。修改都写在 `src/infer/postprocess.cc` 的文件头注释里。
- **追问准备**：
  - NMS 的复杂度？O(n²)，n 是过阈值的候选框数；先用阈值过滤能大幅减少 n。
  - 怎么验证修复？手工构造的例子 + PC 上用模拟 NPU 输出跑通；上板后可以用同一张图对比修复前后的框。
  - 为什么类别之间不互相抑制？不同类别的目标可能重叠（人骑车），按类 NMS 才不会误删；如果要类间抑制可以配置 `class_agnostic`。

### 故事 2：头文件里的 constexpr 静态成员导致链接报"重复定义" ✅

- **S**：重构时把原来只在 `main.cc` 里用的线程池头文件 `ThreadPool.hpp`，同时给推理模块和 App 用。
- **T**：编译通过了，但链接失败：`multiple definition of dpool::ThreadPool::WAIT_SECONDS`。
- **A**：
  1. 看链接错误，重复定义出现在所有包含这个头文件的 `.o` 里；
  2. 查头文件发现：类内有 `static constexpr size_t WAIT_SECONDS = 2;`，类外还写了一行定义 `constexpr size_t ThreadPool::WAIT_SECONDS;`。在 C++14 里，这行类外定义是一个**非 inline 的变量定义**，每个包含它的编译单元都会生成一份，违反 ODR（单一定义规则）；
  3. 原来只有一个 `.cpp` 包含它，所以一直没暴露；
  4. 之所以需要类外定义，是因为 `std::chrono::seconds(WAIT_SECONDS)` 以引用方式使用了它（ODR-use），C++14 要求必须有定义；
  5. 修复：改成 `enum { WAIT_SECONDS = 2 };`——枚举值是纯右值，永远不会被 ODR-use，不需要定义。
- **R**：链接通过。并且理解了 C++17 为什么引入 `inline` 变量：C++17 里 `static constexpr` 数据成员默认就是 inline 的，这个问题自然消失。
- **追问准备**：
  - ODR 是什么？一个程序里非 inline 的函数/变量只能有一个定义；inline 函数、模板、类定义可以在多个编译单元出现但必须完全相同。
  - 还有什么方法修？升级 C++17；或者把定义挪到一个 `.cpp` 里；或者用 `static constexpr` 函数返回值。

### 故事 3：GB28181 PS 流"偶发解码错误"的定位 ✅

- **S**：自己实现了 GB28181 的 PS 封装 + RTP 发送。用一个模拟平台接收 RTP，把负载拼成 PS 文件，用 `ffmpeg -f mpeg` 解码验证。
- **T**：UDP 和 TCP 两种模式抓到的 PS 文件，解码时都报一次 `Invalid level prefix / error while decoding MB`，需要判断是不是封装有 bug。
- **A**：
  1. **先排除容器层**：写脚本解析 PS 结构，检查每个包头起始码、PES 长度、有没有多余字节——结果 76 个 PS 包、PES 长度全部正确、0 个多余字节，容器层没问题；
  2. **再看码流**：把 PES 负载提取成裸 H.264，单独解码，仍然报错，说明问题在码流内容或者截断；
  3. **二分定位出错的帧**：逐步增加解码的帧数，二分找到第一个出错的帧——是第 75 帧，也就是**最后一帧**，大小只有 6.9KB，而且是关键帧（正常关键帧要大得多）；
  4. 结论：模拟平台在 3 秒时停止接收，最后一帧只收到一部分。0~74 帧全部无错误解码。
- **R**：确认封装和分包是正确的，问题是测试方法本身（抓包截断）。之后验证时只解码完整的帧。
- **追问准备**：
  - 为什么先查容器再查码流？分层排查，先确认外层结构，缩小范围。
  - 为什么用二分？帧数多时线性排查太慢，二分只要 log(n) 次。
  - 这个故事想说明什么？"遇到问题先怀疑自己的代码，但要用证据说话；测试方法本身也可能有问题。"

### 故事 4：ThreadSanitizer 报了 5 条数据竞争，全是误报 ✅

- **S**：项目十几个线程，用 TSan 做数据竞争检查。
- **T**：TSan 报了 5 条 data race，要判断真假。
- **A**：
  1. 逐条看报告的**两个栈**：一条是解码线程里 `av_buffer_unref` 的 free 与 FFmpeg 内部线程的写冲突；一条是拼接线程里 `cv::Mat` 析构的 free 与推流线程之前的 `copyTo` 读冲突；
  2. 分析：FFmpeg 的帧缓冲和 OpenCV 的 Mat 都用引用计数管理，计数是用原子指令在**没有插桩的动态库**里完成的，TSan 看不到这些原子操作建立的"先行发生"关系，于是把"最后一个持有者释放内存"误报成与之前的访问冲突；
  3. 核对我们自己的同步：拼接线程和推流线程都是通过 Mbuffer（有锁）拿到 Mat 的浅拷贝，只读，不存在真正的竞争；
  4. 可以用 suppressions 文件屏蔽这些库（`called_from_lib:libavutil.so` 等）。
- **R**：确认我们自己的代码没有数据竞争；也形成了判断 TSan 误报的方法：看冲突两边是不是都在第三方库的引用计数/释放逻辑里、访问的是不是我们自己的成员变量。
- **追问准备**：TSan 的原理？编译时给每次内存访问插桩，运行时用向量时钟记录"先行发生"关系，两个没有先后关系的访问中有一个是写，就报竞争；开销大约 5~15 倍。

### 故事 5：推流服务器重启时进程"无声无息地退出" 🛡️

- **S**：推流线程通过 TCP 往 RTMP/RTSP 服务器写数据。
- **T**：服务器重启或网络断开时，程序要能自动恢复，而不是退出。
- **A**：
  1. 设计时知道：往对端已关闭的 TCP 连接写数据，内核会发 SIGPIPE，默认动作是终止进程，而且**不会留下任何日志**；
  2. 在 `main` 里 `signal(SIGPIPE, SIG_IGN)`，GB28181 的发送额外带 `MSG_NOSIGNAL`，写失败时返回 `EPIPE`，由推流线程处理；
  3. 写失败 → 先正常关闭旧连接（写 trailer / 发 TEARDOWN，避免在服务器上残留会话）→ 指数退避重连 → 连上后强制关键帧；
  4. **测试验证**：用 mediamtx 做服务器，推流中途杀掉服务器，4 秒后重启。
- **R**：日志显示 `Broken pipe` → `connection lost, reconnect` → 服务器恢复后 `publishing to ...`，重新推出的流能正常解码，进程没有退出；同时拉流端也自动重连成功。
- **追问准备**：为什么还要主动关闭旧连接？否则服务器可能认为旧会话还在，新连接推同一路径会被当成重复推流拒绝。

### 故事 6：NV12 的 UV 平面偏移（画面发绿） 🛡️

- **S**：MPP 解码输出的 NV12 帧带 stride，比如 1920×1080 的帧，`hor_stride=1920`、`ver_stride=1088`。
- **T**：CPU 回退路径需要把它转换成 BGR。
- **A**：
  1. 这是 Rockchip 平台的经典坑：UV 平面的起始地址是 `hor_stride × ver_stride`，不是 `宽 × 高`；按宽高算的话，会从 Y 平面最后 8 行填充区开始读色度，画面颜色错位、发绿；
  2. 实现时逐行拷贝：Y 平面拷 `height` 行、每行 `width` 字节，UV 平面从 `hor_stride × ver_stride` 开始拷 `height/2` 行，去掉填充后再 `cv::cvtColor`；
  3. 默认走 RGA 路径，直接把 fd 和 stride 交给 RGA，由硬件处理。
- **R**：RGA 和 CPU 两条路径都按 stride 处理。**上板后建议专门验证一次**：配置 `use_rga = 0` 强制走 CPU 路径，看颜色是否正常。如果你上板时真的遇到过发绿，这个故事就可以改成 ✅ 讲。
- **追问准备**：为什么硬件要对齐？内存访问按总线宽度和 cache line 对齐效率高，编解码器按宏块（16×16）处理图像。

### 故事 7：多线程程序退出时卡死或 terminate 🛡️

- **S**：十几个线程，阻塞点分布在条件变量、FFmpeg 网络 IO、socket、NPU 调用里。
- **T**：Ctrl+C 或 SIGTERM 时要在几秒内干净退出，并且退出时发送 BYE、注销、写 trailer。
- **A**：
  1. 所有线程用 `WorkerThread` 封装：析构时 join，线程入口 catch 所有异常，避免 `std::terminate`；
  2. 所有等待都带超时或能被 `close()` 唤醒；FFmpeg 阻塞调用用 `interrupt_callback` 检测停止标志；
  3. 退出分三步：先通知所有线程并关闭所有队列 → 按生产者到消费者的顺序 join → 最后销毁 NPU 上下文；
  4. 信号处理函数只设原子标志，不在里面做任何复杂操作。
- **R**：PC 上测试 4 路 + 推流场景，SIGINT 后约 20 毫秒内全部线程退出，日志 `pipeline stopped cleanly`；ASan 在退出时没有报告泄漏。
- **追问准备**：见 Part 2 A7。

---

### 讲故事的注意事项

- **重点讲 A（行动）**，尤其是"怎么定位的"：用了什么工具、怎么缩小范围、怎么验证。面试官想看的是你的排查思路。
- **数字要具体**：第几帧、多少字节、多少毫秒，具体数字让故事可信。
- **结尾带一句收获**：比如"从那以后我写头文件里的静态成员都会注意 ODR"。
- **别编**：面试官追问两三层就能分辨真假。🛡️ 类故事如实说是"设计时考虑到并测试验证"，一样能体现能力。


---

## Part 6 项目不足与改进方案

> 面试官几乎一定会问"项目有什么不足 / 如果重新做你会怎么改"。**能主动、具体地说出缺点和改进方案，是"真正做过、真正想过"的最好证明。**
> 回答模板：**现状 → 影响 → 改进方案（以及为什么当时没做）**。挑 2~3 条讲就够，按你面的岗位选（偏系统的挑性能/架构，偏音视频的挑协议/延迟）。

### 一、必须在面试前解决的（不是拿来讲的，是拿来做的）

| 项 | 现状 | 要做什么 |
|---|---|---|
| 板上实测 | 只在 PC 上用模拟 NPU 库 + FFmpeg/OpenCV 回退路径跑通；MPP/RGA/RKNN 只做了编译检查 | 按 [Part 7](#part-7-上板实测清单) 上板实测，拿到帧率、延迟、NPU 占用、内存曲线 |
| GB28181 互通 | 只和自写的模拟平台联调过 | 用 WVP-PRO + ZLMediaKit 实际对接，截图 |
| 融合效果 | 没有精度评测 | 用数据集对比单模型 vs 融合的 precision / recall / mAP，或者准备好诚实说法 |

### 二、架构与性能

#### 1. 每路 3 个线程，扩展性一般
- **影响**：几十路时线程数上百，上下文切换和栈内存开销变大。
- **改进**：拉流改成少量线程 + epoll 事件驱动；解码和推理改成按任务调度的线程池；路数配置和线程数解耦。
- **为什么当时没做**：4 路规模下每路独立线程最简单直观，一路卡住不影响其它路；FFmpeg 的 `av_read_frame` 是阻塞接口，改事件驱动要换 AVIO 自定义读或者自己实现 RTSP。

#### 2. 每帧都新分配 `cv::Mat`，没有缓冲池
- **影响**：1080p BGR 每帧约 6MB，4 路 25fps 就是每秒几百次大块分配，增加 CPU 开销和内存碎片风险。
- **改进**：做一个帧缓冲池，按引用计数判断是否可以复用（引用计数为 1 时说明只剩池子自己持有）；更进一步，全链路用 DMA-BUF，图像不经过 CPU 内存。

#### 3. NPU 输入没有做零拷贝
- **影响**：预处理结果先写进普通内存，`rknn_inputs_set` 再拷贝一次到 NPU 内存。
- **改进**：用 `rknn_create_mem` 分配 NPU 可访问的内存，`rknn_set_io_mem` 绑定为输入，让 RGA 直接把缩放后的图写进去。

#### 4. MPP 编码是同步调用
- **影响**：每帧 `encode_put_frame` 后阻塞等 `encode_get_packet`，推流线程在编码期间什么也做不了。
- **改进**：多缓冲 + 异步：准备 2~3 个输入 buffer 轮流用，put 和 get 放到不同阶段，让颜色转换和编码重叠。

#### 5. CPU 回退路径从 DRM 内存拷贝较慢
- **影响**：RGA 不可用时，CPU 从非缓存的 DRM 内存逐行读 NV12，速度明显慢于普通内存。
- **改进**：使用可缓存的 buffer（配合 `mpp_buffer_sync_*` 做缓存同步），或保证 RGA 路径可用。

### 三、算法与效果

#### 6. 跳帧推理时框有滞后
- **影响**：中间帧复用旧结果，目标快速运动时框跟不上。
- **改进**：中间帧用卡尔曼滤波 / 光流 / 轻量跟踪器（如 ByteTrack 的运动模型部分）预测框的位置，同时还能给目标分配 ID。

#### 7. 融合是一趟贪心关联，而且没做精度评测
- **影响**：结果和排序顺序有关，拥挤场景下可能关联错；融合到底提升多少不知道。
- **改进**：关联改成匈牙利算法（全局最优匹配）；建立评测集，对比不同 `method` / `min_votes` 的 precision/recall 曲线来选参数。

#### 8. 后处理只支持 YOLOv5（int8、NCHW、固定 anchor）
- **改进**：把后处理做成接口，按模型类型注册（YOLOv8 无 anchor、分割、姿态等）；anchor 从配置读取。

### 四、协议与稳定性

#### 9. GB28181 功能有限
- 同一时刻只支持一路点播会话，目录只上报一个通道；SIP 只支持 UDP；只支持 H.264 的 PS；Digest 只实现了 `qop=auth`。
- `write()` 持锁时如果 TCP 发送阻塞，最长 2 秒，期间 SIP 线程处理信令会被拖住。
- **改进**：多会话、多通道（每路一个通道 ID）；SIP over TCP；H.265（PSM 的 stream_type 改为 0x24）；媒体发送放到独立队列/线程，锁里只拷贝会话信息。
- **为什么自己实现**：见 Part 2 F4。生产环境可以考虑直接用 ZLMediaKit 的国标模块。

#### 10. 实时流重连后内部时间戳会从 0 重新开始（已知 bug）
- **现状**：`StreamLoader` 重连时把 `first_pts_` 重置了，但只有文件循环时才更新 `pts_offset_`，所以实时流重连后这一路帧的 pts 从 0 重新开始，不再单调递增。
- **影响**：目前推流用自己的时钟，**不影响推流时间戳**；但如果以后用帧 pts 做同步或统计，就会出错。
- **改进**：重连时也把 `pts_offset_` 设为 `last_pts_ + 一帧时长`（几行代码）。

#### 11. 硬件回退后不再重试
- **现状**：某个实例的 RGA 调用失败一次，这个实例就一直走 CPU。
- **改进**：记录失败次数和时间，隔一段时间再试一次 RGA；或者按错误码区分"永久不支持"（对齐不满足）和"临时失败"。

#### 12. 有两处无超时等待，没有看门狗
- **现状**：等空闲模型实例（`Lease`）和等推理结果（`future.get()`）没有超时，如果 NPU 驱动卡死，推理线程会跟着卡住。
- **改进**：`wait_for` + 超时告警；加一个看门狗：perf 线程检查各路是否还在出帧，正常才调用 systemd 的 `sd_notify("WATCHDOG=1")`，卡死时由 systemd 重启进程。

#### 13. 没有贯穿全链路的延迟统计
- **现状**：Mbuffer 在每一级写入时都刷新时间戳，没有从解码一路带到输出的时间戳。
- **改进**：`FrameData` 增加"解码完成时间"字段，一路透传，在推流前计算端到端处理延迟，放进性能报告和 OSD。

### 五、工程化

#### 14. 没有自动化单元测试
- **现状**：验证靠集成测试：PC 上用模拟 NPU 库跑全流程、mediamtx 测推流和重连、模拟国标平台测信令、ASan/TSan 检查。
- **改进**：用 gtest 给纯逻辑模块写单元测试——融合算法、PS 封装（和 FFmpeg 解析结果对比）、SIP 报文解析、配置解析、FpsController；在 CI 里用 PC 构建 + 模拟库跑冒烟测试和 ASan。

#### 15. 配置不能热更新，没有管理接口
- **改进**：加一个 HTTP 管理接口（查询状态、统计、动态开关推流、调整阈值）；配置修改后对单个模块热重启。

---

### 反问环节（面试最后"你有什么要问我的"）

挑 1~2 个，体现你关心工作内容和成长：

- 团队目前的产品主要跑在什么平台上？是 Rockchip 还是其它 SoC（海思、全志、Jetson）？
- 这个岗位日常更偏哪一块：多媒体管线、设备接入协议，还是系统层（驱动适配、性能优化）？
- 团队怎么做性能和稳定性测试？有没有长时间压测或故障注入的环境？
- 新人入职一般从哪类任务开始上手？


---

## Part 7 上板实测清单

> 目的：拿到**自己亲手测的、可复现的**数据，填进 Part 1 的项目介绍和简历。面试官问"跑多少帧、延迟多少、NPU 占用多少"时，能说出具体数字和测试条件。
> 所有带 `sudo` 的命令需要 root，`/sys/kernel/debug` 需要挂载 debugfs（Rockchip 官方镜像一般默认挂载）。

### 1. 记录测试环境

测试结果离开环境没有意义，先把这些记下来：

```bash
cat /proc/device-tree/model                                   # 板子型号
free -h                                                       # 内存大小
uname -r                                                      # 内核版本
strings install/rknn_multi_stream_Linux/lib/librknnrt.so | grep "librknnrt version"   # RKNN runtime 版本
strings install/rknn_multi_stream_Linux/lib/librga.so | grep "rga_api version"        # librga 版本
sudo cat /sys/kernel/debug/rknpu/version 2>/dev/null          # NPU 驱动版本(节点不存在可以 dmesg | grep -i rknpu)
dpkg -l | grep -i -E "mpp|rga|ffmpeg|opencv"                  # 相关软件包版本
cat /sys/class/devfreq/fdab0000.npu/cur_freq                  # NPU 频率
```

| 项 | 值 |
|---|---|
| 板子型号 / 内存 | |
| 内核版本 | |
| librknnrt 版本 | 仓库自带为 1.5.2 |
| librga 版本 | 仓库自带为 1.9.1 |
| NPU 驱动版本 | |
| MPP / FFmpeg / OpenCV 版本 | |
| 是否运行 performance.sh 定频 | 是 / 否 |
| 模型 | yolov5s-640-640.rknn（relu，int8） |
| 输入源 | 例如：4 路本地 1080p25 H.264 文件 / 4 路海康 RTSP 主码流 |

### 2. 测试矩阵

每组跑 **至少 5 分钟**，取稳定后的数据。建议先测文件输入（可复现），再测真实摄像头。

| 编号 | 路数 | 分辨率 | 模型数 | infer_interval | target_fps | 推流 |
|---|---|---|---|---|---|---|
| T1 | 1 | 1080p | 1 | 1 | 0（不限） | 关 |
| T2 | 4 | 1080p | 1 | 1 | 0 | 关 |
| T3 | 4 | 1080p | 1 | 1 | 25 | 关 |
| T4 | 4 | 1080p | 2 | 1 | 25 | 关 |
| T5 | 4 | 1080p | 1 | 2 | 25 | 关 |
| T6 | 4 | 1080p | 1 | 1 | 25 | 拼接画面 RTMP |
| T7 | 8 | 1080p | 1 | 2 | 15 | 关（摸上限） |

**要点**：T1 看单路极限；T2 看 4 路的瓶颈在哪；T3~T5 对比跳帧、多模型的效果；T6 看推流开销；T7 摸路数上限。

### 3. 每组要采集的数据

#### 3.1 帧率与各阶段耗时：看程序自带的性能报告

`config/app.ini` 里设 `perf_interval = 5`，日志中每 5 秒打印一次：

| 数据 | 看哪一行 |
|---|---|
| 每路输出帧率 | `chN.out` 的 rate/s |
| 每路推理帧率 | `chN.infer` 的 rate/s |
| 单次推理总耗时 | `chN.infer` 的 avg(ms) |
| NPU 纯推理耗时 | `<模型名>.npu` 的 avg(ms) |
| 预处理 / 后处理耗时 | `<模型名>.pre` / `.post` |
| 等实例耗时（实例不够时变大） | `<模型名>.wait` |
| 解码 / 颜色转换耗时 | `chN.decode` / `chN.yuv2bgr` |
| 丢帧 | `chN.drop.pkt`（解码跟不上）/ `chN.drop.frame`（推理跟不上）/ `chN.drop.fps`（帧率控制主动丢） |
| 编码耗时 | `pushN.encode` |
| 瓶颈提示 | 最后的 `bottleneck(load>85%)` 行 |

同时看启动日志，确认**硬件路径真的生效了**：
- 解码器是 `decoder 'mpp' created`，不是 `ffmpeg`；
- 没有 `rga ... fallback to opencv/cpu` 的警告；
- 编码器是 `mpp h264 encoder ready`，不是 `ffmpeg encoder 'libx264'`。

#### 3.2 NPU、CPU、温度

```bash
PID=$(pidof rknn_multi_stream)
sudo cat /sys/kernel/debug/rknpu/load                 # 三个 NPU 核的占用
top -H -b -n 1 -p $PID | head -30                     # 每个线程的 CPU(线程名可见)
top -b -n 1 | head -5                                 # 整机 CPU
cat /sys/class/thermal/thermal_zone*/temp             # 温度(毫摄氏度), 过热会降频导致帧率下降
```

#### 3.3 内存（长时间）

```bash
PID=$(pidof rknn_multi_stream)
# 每 60 秒记录一行: 时间, RSS(KB), 线程数, fd 数, CMA 可用(KB)
while kill -0 $PID 2>/dev/null; do
  echo "$(date +%T),$(awk '/VmRSS/{print $2}' /proc/$PID/status),$(awk '/Threads/{print $2}' /proc/$PID/status),$(ls /proc/$PID/fd | wc -l),$(awk '/CmaFree/{print $2}' /proc/meminfo)"
  sleep 60
done > mem.csv
```

判断标准：跑够时间后，RSS、线程数、fd 数、CMA 应该是**平的**（启动后几分钟内上涨是正常的）。

#### 3.4 端到端延迟（"玻璃到玻璃"）

1. 在手机或电脑上打开一个显示毫秒的秒表；
2. 让摄像头拍这个秒表；
3. 把程序的显示窗口（或推流播放器）和秒表放在一起，用另一部手机**同时拍下两者**；
4. 两个时间相减就是端到端延迟。重复拍 5~10 次取平均。

分别测：本地显示的延迟、RTSP 推流后用 `ffplay -fflags nobuffer -flags low_delay` 播放的延迟。播放器的缓冲会显著影响结果，测试时要说明用的是什么播放器、什么参数。

### 4. 稳定性与故障注入

| 测试 | 操作 | 期望 | 结果 |
|---|---|---|---|
| 长时间运行 | 4 路 + 推流，运行 24 小时，同时跑 3.3 的记录脚本 | 不崩溃；RSS/线程/fd/CMA 平稳 | |
| 摄像头断开 | 拔掉一路摄像头网线 30 秒再插回 | 该格显示 RECONNECTING，其余路不受影响；恢复后自动出画面 | 记录恢复时间 |
| 推流服务器重启 | 重启 RTMP/RTSP 服务器 | 进程不退出，服务器恢复后自动重新推流 | 记录恢复时间 |
| 国标平台重启 | 重启 WVP | 心跳超时后自动重新注册，平台能重新点播 | |
| 重复推流 | 用 ffmpeg 先推同一个 RTSP 路径 | 日志提示路径被占用，退避重试，不崩溃 | |
| 优雅退出 | `kill -TERM $PID` | 几秒内退出，日志有 `pipeline stopped cleanly` | 记录退出耗时 |
| 板上 ASan | 用 ASan 编译运行 10 分钟后 Ctrl+C | 无错误、无泄漏报告 | 截图保存 |

板上 ASan 编译（需要安装 `libasan`）：

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j8
```

### 5. GB28181 真实平台对接（强烈建议）

1. 用 Docker 部署 WVP-PRO + ZLMediaKit（网上有现成的一键部署脚本）；
2. 在 WVP 里记下 SIP 服务器 ID、域、IP、端口、密码，填进 `[push2]`；
3. 设 `enable = 1`、`log_level = debug`，启动程序，看 WVP 设备列表里设备是否在线、通道是否出现；
4. 在 WVP 上点播，看能否出画面；分别试 UDP 和 TCP 两种传输；
5. 截图：设备在线、通道列表、播放画面；同时 `tcpdump` 抓一份信令包留着讲解用。

遇到问题优先看：SSRC 是否一致、平台收流端口是否可达、TCP 主被动模式是否匹配（见学习指南 12.10）。

### 6. 结果汇总表（填好后用于简历和面试）

| 场景 | 每路帧率 | 推理耗时 | NPU 三核占用 | 整机 CPU | 端到端延迟 | RSS |
|---|---|---|---|---|---|---|
| T1 单路极限 | | | | | | |
| T2 4 路不限帧 | | | | | | |
| T3 4 路 25fps | | | | | | |
| T4 4 路双模型 | | | | | | |
| T5 4 路跳帧 | | | | | | |
| T6 4 路 + 推流 | | | | | | |
| T7 8 路 | | | | | | |

**面试时这样说数字**："在 RK3588、4 路 1080p25 H.264 输入、yolov5s int8 的条件下，每路稳定 xx fps，NPU 三个核占用分别约 xx%，整机 CPU 约 xx%，本地显示端到端延迟约 xx 毫秒；24 小时运行内存平稳。"——**条件 + 数字 + 稳定性**，缺一不可。

