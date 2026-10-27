# 官方开发包导读

九鼎给的 V821 开发包很大（源码 tarball 3.0G，解压 4.7G，128 份 PDF）。这份文档是**导航**：
移植过程中遇到的问题，各自该去官方包的哪个位置找答案。

> **只给路径和结论，不搬运厂商源码。** 全志/九鼎 SDK 的代码和文档有自己的授权条款，
> 这里不粘贴它们的代码片段、头文件签名和参数表 —— 按下面的路径去看原文。
> 文中的实测数字（耗时、命中率、mixer 设定值等）是我们在这块板上量出来的结果。

---

## 1. 东西在哪

假设开发包解压在 `~/kit/v821-tina-v12`（下称 SDK 根）。

| 内容 | 位置 |
|---|---|
| 源码 tarball | 光盘 `source/v821.tar.gz`（3.0G） |
| 官方预编译固件 | 光盘 `image/v821_linux_aitoy_*.img`，板子出厂跑的就是它 |
| PhoenixSuit 烧录工具 + USB 驱动 | 光盘 `tools/` |
| 芯片手册 / 引脚表 / 主板硬件手册 | 光盘 `datasheet/`、`usermanual/` |
| **SDK 完整文档集（128 份 PDF + HTML）** | SDK 根 `docs/{pdf,html}/` —— 比光盘那份总手册好用得多 |
| 交叉工具链 | SDK 根 `prebuilt/rootfsbuilt/riscv/nds32le-linux-musl-v5d.tar.xz` |
| 板级配置（dts / 分区表 / defconfig） | `device/config/chips/v821/configs/aitoy/` |
| **小智 Linux 版 demo**（本项目的主要参考） | `platform/allwinner/llm/chatbox/xiaozhi/` |
| 火山引擎 RTC demo（含视频，`.so` 闭源） | `platform/allwinner/llm/chatbox/volcengite/` |
| MPP 中间件（源码 + 47 个 sample） | `platform/allwinner/eyesee-mpp/middleware/sun300iw1/` |
| 视觉算法包（人形检测 / 运动侦测） | `openwrt/package/allwinner/vision/` |

### 文档集里真正会用到的几份

`docs/pdf/board/` 下按模块分：

| 文档 | 什么时候看 |
|---|---|
| `MPP/Tina_Linux_MPP_AEC回声消除_调试指南.pdf` | 走官方 AEC 那条路 |
| `MPP/Tina_Linux_MPP_Sample_使用说明.pdf` | 跑 §4 那些 sample |
| `MPP/Tina_Linux_多媒体MPP_开发指南.pdf` | 视频方案的 API 总纲 |
| `MPP/V821_Tina_Linux_多媒体内存_优化指南.pdf` | 只有 43.8MB 可见内存，做视频必看 |
| `WiFi_BT/Tina_Linux_蓝牙_开发指南.pdf` | BLE 配网：XRADIO 支不支持 LE peripheral |
| `ISP/V821_Tina_Linux_Camera_通路配置指南.pdf` | 摄像头换分辨率 / 换 sensor |
| `board/V821_Tina_Linux_SDK_使用指南.pdf` | 全量编译 + 刷机步骤 |
| `board/V821_Tina_Linux_方案FAQ.pdf` | 踩坑先查这个 |

---

## 2. 音频：官方 demo 和我们的做法不一样

官方 `chatbox/xiaozhi/port/audio_codec.c` 走 **MPP 的 AO/AI 通道**（`AW_MPI_AO_*` /
`AW_MPI_AI_*`），不是 ALSA。要照它做就去读那个文件，这里只记三条结论：

- **`port/audio_process.c` 是个空壳**（每个函数体只有一个 `;`）。也就是说官方 demo
  **完全没有应用层音频处理** —— VAD / AEC / 降噪全在 MPP 里。这解释了为什么我们在
  arecord/aplay 管道上叠 speexdsp 那么费劲：不是参数没调对，是这套架构拿不到时间
  对齐的参考信号（量化理由见主 README §8）。
- **播放需要单独的时钟通道**，和 AO 通道的 start/stop 成对出现，少了就不出声。
- **暂停采集时要连 AEC/ANS 一起挂起**，恢复时反序恢复。

我们没走 MPP，原因见 §3。

---

## 3. 关于 AEC 的两条路

### 「硬件 AEC」是误称

官方那个 AEC 开关背后是 **MPP 里的软件算法**（WebRTC AEC 和另一套第三方 ECNR 的封装，
源码在 `middleware/sun300iw1/media/audio/aec/`，预编译库在
`middleware/sun300iw1/media/LIBRARY/aec_lib/`）。板级 dts 的 codec 节点里
**没有任何 aec/echo 相关属性**，印证了这一点。

MPP可在同一音频链路中提供采音与播放参考，仍需要实测对齐、延时和漂移。
旧记录曾将`MAX_DELAY`宏直接解释为毫秒上限，并用管道容量代替固定声学延时；
该解释不足以支持量程结论。当前采用进程内小缓冲I/O，按实际排队深度估算延时。

### 两条路的取舍

| 路线 | 说明 |
|---|---|
| **A. 进程内 ALSA + SDK WebRTC AEC** | `aec_lib/webrtc/library/musl/`静态库已验证目标为riscv32 + musl，可与进程内ALSA链接。当前实现记录实际播放排队深度并估计参考延时；仍需在目标硬件验证真实声学对齐与双讲质量。库和头文件需开发者自行从合法取得的SDK准备，不随本仓库分发 |
| **B. 走 MPP AO/AI** | MPP 是**全静态链接**（见 `chatbox/xiaozhi/mpp_config.mk` 的静态库列表），板上没有对应 `.so` 可蹭 —— 这也解释了为什么固件的 `/usr/lib/eyesee-mpp/` 几乎是空的。必须先跑一遍 SDK 构建把 staging 里的 `.a` 生成出来（15–25G 磁盘）。生成完可以用我们自己的 CMake 链，**不一定要刷机**，除非同时要改 dts |

A路线已作为可选`V821_AEC_BACKEND=webrtc`接入。原半双工及Speex构建保留，
配置、恢复策略和验证范围见[全双工开发说明](voice-aec-wakeword.md)。

---

## 4. 视觉：V821 **没有 NPU**

### 结论

- 官方芯片手册全文检索 `tops` / `npu` / `neural` / `算力` **零命中**；中英文 Brief 同样零命中。
- Brief 的功能框图里没有 NPU/NNA 模块，只有 RISC-V CPU 1GHz + RISC-V MCU 600MHz、ISP、
  **H.264/JPEG 硬编码**、DE、**G2D**、Audio Codec、Wi-Fi、SiP 64MB DDR2、12 路 PWM。
- 芯片定位原文是 *"low-power multi-camera IPC SoC"* —— 网络摄像机芯片。
- SDK 里确实有 `bsp/drivers/npu/`，但那是**全志各芯片共用的 BSP 树**（Kconfig 里
  `default n`，V821 的任何 defconfig 都没开），而且整个 SDK **没有配套的用户态运行时**
  （没有模型编译器、没有 OpenVX 运行时）。没有用户态就跑不了任何模型 —— 那个目录是残留，
  不是能力。

如果有人跟你说这颗芯片有 0.5 TOPS，先让他给出处。

### 但有可用的 on-device AI —— 跑在 CPU 上

`openwrt/package/allwinner/vision/` 下有**预编译好的 riscv32 + musl 静态库**：

| 包 | 内容 |
|---|---|
| `person_detection/` | YOLO 人形检测，输入 320×192×3，含权重和一个自带计时的 demo |
| `motion_detection/` | 运动侦测，比人形检测便宜得多 |

推理内核是 CMSIS-NN 风格的**纯标量 RISC-V** 实现，编译参数里也没有向量扩展 ——
纯 CPU 软推理，再次印证没有 NPU。

**本项目实测**（`--pdet-tune`，320×192）：

| 指标 | 值 |
|---|---|
| 单帧耗时（连着跑） | **167ms** |
| 单帧耗时（稀疏采样） | **约 330ms** —— CPU 调频没爬上来，估算 CPU 占用要按这个算 |
| 有人时命中率 | **~100%**（预处理修好之后） |
| 空场景误检率 | **~9%**，且都是孤立单帧 |

预处理踩过的坑（这两条把召回从 37% 拉到 ~100%，细节见主 README §5.7）：

- 缩放要用**面积平均**，不能用最近邻 —— 640×480 → 320×192 是 2×2.5 倍下采样，
  最近邻会丢掉 60% 的行，人的边缘被采样噪声打碎
- 要**letterbox 保宽高比** —— 相机 4:3 直接压到模型的 5:3 会把人竖着挤扁 1.25 倍

接线很顺：视觉包自带的图像工具库有 YUV→RGB 和缩放函数，正好接 V4L2 出来的 NV21 帧。

### 本地唤醒词：官方包里这条路是死的

`eyesee-mpp/external/uvoice/` 下那两个唤醒词库和 `sound_controler/lib/` 里的库
**都是 ARM 的**（`file` 一看就知道），是从别的芯片方案带过来的残留，V821（riscv32）
上根本加载不了。模型文件在，但没有能跑它的 riscv32 库。

`aec_lib/uvoice/` 下有个 riscv32 的第三方 ECNR 库（回声消除+降噪，**不是**唤醒词），
而且需要授权。

要真做本地唤醒：找九鼎要 riscv32 版 KWS 库，或者自己上 sherpa-onnx 之类 —— 但人形检测
已经占掉可观的 CPU，得先算账。

---

## 5. 视频方案：相关的 sample

`middleware/sun300iw1/sample/` 共 47 个，相关的：

| sample | 用途 |
|---|---|
| `sample_virvi` | 摄像头取流（我们直接用 V4L2 了，可对照它的参数） |
| **`sample_virvi2venc`** | **取流 → H.264/265 硬编码 ← 视频方案的起点** |
| `sample_virvi2venc2muxer` | 取流 → 编码 → 封装 MP4 |
| `sample_rtsp` | RTSP 推流 |
| `sample_OnlineVenc` | 在线编码，码率自适应 |
| `sample_takePicture` | 拍照 |
| `sample_aec` / `uncommonly_samples/sample_ao2ai_aec` | AEC 环路（见 §3） |
| `sample_identifyQRCode` | 二维码识别 |
| `SampleManual.md` | 所有 sample 的说明 |

**但瓶颈不在这边**：涂鸦视频通话要 `rtc-client` 的预编译库，官方没出 riscv32 版
（见主 README §7）。硬编能力和 sample 都在，等涂鸦给库。

---

## 6. 板级配置里那两个坑的源头

`device/config/chips/v821/configs/aitoy/linux-5.4-ansc/board.dts` 的 codec 节点：

- 打开了 **rx sync** —— 于是普通 `arecord` 会直接报 I/O error，必须在用户态关掉
- **麦克风增益和 lineout 增益的取值超出了驱动的掩码范围**，被截断成很小的值 ——
  这就是为什么录音电平只有 -44dBFS，得靠 +30dB 数字增益补

改 dts 要重编内核 + 刷机；用 `amixer` 在用户态改不用。我们选了后者，实测出来的配方在
`bsp/boards/v821_aitoy/board_config.h`，每一项的理由见主 README §5。

---

## 7. 官方 demo 里我们**没有**复用的部分

| 官方有 | 为什么不用 |
|---|---|
| `chatbox/xiaozhi/core/` 整套（websocket / protocol / ota / iot） | 那是对接小智服务器的协议栈，我们走涂鸦 TAI，agentic-kit 里全有 |
| `chatbox/xiaozhi/core/posix/`（task/mutex/queue 抽象） | 只跑 Linux，pthread 直接用；agentic-kit 的 `pal_t` 已经是这层 |
| `libstdopus/`（opus 源码） | 我们请求云端发 **PCM**，不解 opus（顺带把首音频延迟从 3.9s 压到 1.2s） |
| `volcengite/` | 火山引擎 RTC，闭源 `.so`，不是我们的云 |
| `chatbox/xiaozhi/port/display.c` | 走 sunxifb + G2D 双缓冲；我们的 `display_fb.c` 已跑通，等有性能问题再看 G2D 加速 |
