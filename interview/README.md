# 面试准备资料（Linux 应用开发方向）

本文件夹整理了拿这个项目去面 **Linux 应用 / 嵌入式 Linux 应用 / 音视频应用** 岗位时，面试官最可能问的问题和参考回答。

> 配套阅读：[`../docs/LEARNING_GUIDE.md`](../docs/LEARNING_GUIDE.md)（全链路图解 + 调试方法）。这里侧重"怎么答"，那边侧重"怎么懂"。

## 文件目录

| 文件 | 内容 | 什么时候看 |
|---|---|---|
| [01-project-pitch.md](01-project-pitch.md) | 30 秒 / 1 分钟 / 3 分钟项目介绍、简历写法 | 最先背熟 |
| [02-project-deep-dive.md](02-project-deep-dive.md) | 项目深挖问答（架构、多线程、音视频、推理、推流、国标、稳定性、扩展） | 重点，反复过 |
| [03-linux-cpp-basics.md](03-linux-cpp-basics.md) | 和项目强相关的 Linux / C++ 八股，每题附"结合项目怎么说" | 刷题 |
| [04-coding.md](04-coding.md) | 手撕代码：阻塞队列、线程池、交替打印、shared_ptr、LRU、NMS、位操作等（均已编译测试） | 动手写，每题限时 |
| [05-stories.md](05-stories.md) | "遇到过什么难题"——踩坑故事（STAR 格式），标注了哪些是真实发生的 | 背熟 3 个 |
| [06-weaknesses-and-improvements.md](06-weaknesses-and-improvements.md) | 项目不足与改进方案、反问环节 | 面试前一天 |
| [07-benchmark-checklist.md](07-benchmark-checklist.md) | 上板实测清单和数据记录表 | **上板后立刻做** |

## 面试官一般怎么考

```
 项目深挖  ██████████████████████████  约一半时间   从简历最亮的点往下挖, 挖到你答不上来为止
 基础八股  ███████████████             约三成       多线程 / 网络 / 内存 / 信号 / C++, 常用项目举例追问
 手撕代码  ██████████                  约两成       生产者消费者、线程池、字符串/链表、位操作
```

（比例是经验值，不同公司差异很大。）

## 面试前必须完成的 5 件事

- [ ] **上板实测**：每路帧率、NPU 三核占用、各线程 CPU、端到端延迟、内存曲线（见 07）。**没有实测数字，一问就露馅。**
- [ ] **能讲清每一处设计的"为什么"**：面试官会随机指一段代码问你为什么这么写。
- [ ] **跑一次 ASan 并截图**：证明"没有内存泄漏"不是空口说。
- [ ] **（强烈建议）用 WVP-PRO + ZLMediaKit 实际对接一次 GB28181**，截图平台上的画面。目前只和自写的模拟平台联调过。
- [ ] **融合效果**：要么在数据集上测一组对比（单模型 vs 融合），要么准备好诚实的说法（"重点是工程框架，融合策略可配置，还没系统评测"）。**不要编数字。**

## 高频问题 Top 20（按被问概率排序）

1. 项目整体架构讲一下，有几个线程，线程之间怎么通信？ → [02 A1-A3](02-project-deep-dive.md#a-架构与多线程)
2. 手写一个生产者消费者 / 阻塞队列。 → [04 题 1](04-coding.md)
3. 下游处理不过来怎么办？为什么用两种缓冲区？ → [02 A4](02-project-deep-dive.md#a-架构与多线程)
4. 条件变量为什么要用 while / 谓词？ → [03 1.2](03-linux-cpp-basics.md)
5. 板子上实际能跑多少帧？NPU 利用率多少？延迟多少？ → [07](07-benchmark-checklist.md)
6. 内存泄漏怎么排查？ → [02 E1](02-project-deep-dive.md#e-稳定性与调试)
7. 程序怎么安全退出？std::thread 析构会发生什么？ → [02 A7](02-project-deep-dive.md#a-架构与多线程)
8. 死锁的条件，你的线程池为什么不会死锁？ → [02 A6](02-project-deep-dive.md#a-架构与多线程)
9. I/P/B 帧、GOP、SPS/PPS；为什么丢包要丢到关键帧？ → [02 B1-B2](02-project-deep-dive.md#b-音视频与-rockchip-硬件)
10. NV12 格式和 stride；零拷贝是什么？ → [02 B4-B6](02-project-deep-dive.md#b-音视频与-rockchip-硬件)
11. SIGPIPE 是什么？推流断开进程为什么会退出？ → [02 D4](02-project-deep-dive.md#d-推流与-gb28181)
12. select / poll / epoll 的区别。 → [03 3.1](03-linux-cpp-basics.md)
13. NPU 三个核怎么用起来的？ → [02 C1-C2](02-project-deep-dive.md#c-推理与融合)
14. RTSP 交互流程；RTP over TCP 和 UDP 的区别。 → [02 D1](02-project-deep-dive.md#d-推流与-gb28181)
15. GB28181 注册和点播流程。 → [02 D5-D6](02-project-deep-dive.md#d-推流与-gb28181)
16. 手写线程池。 → [04 题 2](04-coding.md)
17. 遇到过最难的问题是什么？ → [05](05-stories.md)
18. 如果要支持 16 / 32 路怎么改？ → [02 F1](02-project-deep-dive.md#f-扩展与设计权衡)
19. malloc 的原理，RSS 和 VSZ 的区别。 → [03 4.x](03-linux-cpp-basics.md)
20. 项目有什么不足？ → [06](06-weaknesses-and-improvements.md)
