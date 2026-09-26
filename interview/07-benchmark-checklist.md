# 07 上板实测清单

> 目的：拿到**自己亲手测的、可复现的**数据，填进 01 的项目介绍和简历。面试官问"跑多少帧、延迟多少、NPU 占用多少"时，能说出具体数字和测试条件。
> 所有带 `sudo` 的命令需要 root，`/sys/kernel/debug` 需要挂载 debugfs（Rockchip 官方镜像一般默认挂载）。

## 1. 记录测试环境

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

## 2. 测试矩阵

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

## 3. 每组要采集的数据

### 3.1 帧率与各阶段耗时：看程序自带的性能报告

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

### 3.2 NPU、CPU、温度

```bash
PID=$(pidof rknn_multi_stream)
sudo cat /sys/kernel/debug/rknpu/load                 # 三个 NPU 核的占用
top -H -b -n 1 -p $PID | head -30                     # 每个线程的 CPU(线程名可见)
top -b -n 1 | head -5                                 # 整机 CPU
cat /sys/class/thermal/thermal_zone*/temp             # 温度(毫摄氏度), 过热会降频导致帧率下降
```

### 3.3 内存（长时间）

```bash
PID=$(pidof rknn_multi_stream)
# 每 60 秒记录一行: 时间, RSS(KB), 线程数, fd 数, CMA 可用(KB)
while kill -0 $PID 2>/dev/null; do
  echo "$(date +%T),$(awk '/VmRSS/{print $2}' /proc/$PID/status),$(awk '/Threads/{print $2}' /proc/$PID/status),$(ls /proc/$PID/fd | wc -l),$(awk '/CmaFree/{print $2}' /proc/meminfo)"
  sleep 60
done > mem.csv
```

判断标准：跑够时间后，RSS、线程数、fd 数、CMA 应该是**平的**（启动后几分钟内上涨是正常的）。

### 3.4 端到端延迟（"玻璃到玻璃"）

1. 在手机或电脑上打开一个显示毫秒的秒表；
2. 让摄像头拍这个秒表；
3. 把程序的显示窗口（或推流播放器）和秒表放在一起，用另一部手机**同时拍下两者**；
4. 两个时间相减就是端到端延迟。重复拍 5~10 次取平均。

分别测：本地显示的延迟、RTSP 推流后用 `ffplay -fflags nobuffer -flags low_delay` 播放的延迟。播放器的缓冲会显著影响结果，测试时要说明用的是什么播放器、什么参数。

## 4. 稳定性与故障注入

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

## 5. GB28181 真实平台对接（强烈建议）

1. 用 Docker 部署 WVP-PRO + ZLMediaKit（网上有现成的一键部署脚本）；
2. 在 WVP 里记下 SIP 服务器 ID、域、IP、端口、密码，填进 `[push2]`；
3. 设 `enable = 1`、`log_level = debug`，启动程序，看 WVP 设备列表里设备是否在线、通道是否出现；
4. 在 WVP 上点播，看能否出画面；分别试 UDP 和 TCP 两种传输；
5. 截图：设备在线、通道列表、播放画面；同时 `tcpdump` 抓一份信令包留着讲解用。

遇到问题优先看：SSRC 是否一致、平台收流端口是否可达、TCP 主被动模式是否匹配（见学习指南 12.10）。

## 6. 结果汇总表（填好后用于简历和面试）

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
