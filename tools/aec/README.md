# V821 AEC 再验证

独立实验入口，不连接云端、不改设备凭据，也不改变 `v821_chatbot` 的半双工行为。
用于单独验证持续采播、算法和延时；已集成的产品链路见
[全双工开发说明](../../docs/voice-aec-wakeword.md)。

## 当前证据

2026-10-26：Mac arm64 主机编译和 7 项离线检查通过。SpeexDSP 1.2.1 对合成的
40ms 延迟、两径线性回声，在后 5 秒窗口测得衰减约 27.8dB；合成双讲的近端
投影增益约 0.807，输出相对近端单独经过同一 AEC 的基准 SNR 约 24.8dB。
没有开降噪，避免把“压低所有声音”误判成消回声。Speex 本身有 DC-notch，
双讲误差和经过同一滤波的近端基准比较，不能直接拿未滤波的输入算损伤。

独立工具现已在恢复的 V821 Linux SDK 上交叉构建，并核对 ELF32 RISC-V / ilp32d / musl，
运行时动态依赖仅 `libc.so`；strip后445,388字节。独立工具已在板上连续采播5秒/500帧，
16k单声道、10ms period、40ms buffer、gain25%、关闭降噪，退出0且无XRUN；AEC平均
1,318us/帧、最大1,569us/帧。原始录音留板，仅在板上计算数字指标。
**真实回声/双讲质量与实人插话时延仍未验收。** 产品连云检查及中断后采集余量修复见
[开发说明](../../docs/voice-aec-wakeword.md)。

## 设计与边界

- 同进程通过 alsa-lib 打开采集和播放，不再经过 `arecord/aplay` 子进程管道。
- 固定 16kHz、单声道、S16_LE，算法帧长 10ms。请求 10ms period、40ms buffer，
  打印实际协商结果；拒绝超过 100ms 或不能维持至少两个 period 的缓冲配置。
- 先填满静音缓冲，再尝试联动启动；不支持时顺序启动并明确记录。**同进程和
  联动启动都不等于已经证明参考与回声对齐**，仍需从录音测量延时及漂移。
- 麦克风整段保持工作；播放文件结束后继续送零参考并采集至少 1 秒尾音。
- 参考为实际提交给声卡、已应用软件音量的 PCM；逻辑上对应同一轮循环，
  不假定与麦克风在同一时刻到达，自适应滤波器覆盖剩余延时。
- 默认只跑 AEC，`--denoise` 可单独对照降噪，AGC 关闭。
- 输出三路 PCM 和每帧 CSV；I/O 超时或 XRUN 立即中止，不静默恢复后宣布成功。
- 不设置 mixer、不停止其他应用、不启动守护进程，也不启用 VAD/打断。
  声卡被占用会报错退出；实验前通过正常方式停止占用它的应用。
- 录音可能包含人声，以 0600 权限新建，已有文件不覆盖。录音放在 `build/`
  或板上的独立实验目录，别提交到 Git。

## 主机离线构建

准备当前主机架构的 SpeexDSP。`SPEEX_ROOT` 是安装前缀，不是源码目录。
依赖源码和产物可放在被忽略的 `build/aec-deps/`。

```sh
cmake -S tools/aec -B build/aec \
  -DSPEEX_ROOT="$PWD/build/aec-deps/speexdsp-install" -DAEC_WITH_ALSA=OFF
cmake --build build/aec -j
ctest --test-dir build/aec --output-on-failure -V
```

Apple Silicon 若 CMake 在 Rosetta 下运行，还需 `-DCMAKE_OSX_ARCHITECTURES=arm64`，
确保程序和 SpeexDSP 架构一致。主机可执行文件不能部署到 V821。

重放一组等长、同采样率和通道的录音；末帧不足 10ms 时补零计算，输出保留
原样本数。所有输入都是裸 PCM，不能直接传 WAV/MP3。

```sh
build/aec/v821_aec_probe replay \
  --mic build/run01.mic.raw --ref build/run01.ref.raw --out build/run01.replay.raw
```

## V821 构建与采集

在 Linux SDK 构建机准备 **riscv32 / ilp32d / musl** 的 SpeexDSP、alsa-lib 头文件
和库。不得使用 Mac、ARM、aarch64 或 glibc 库。本实验独立配置，不需要主程序的
LVGL、相机库、涂鸦 SDK 或三元组。

```sh
cmake -S tools/aec -B build/aec-v821 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain-v821.cmake" \
  -DV821_TOOLCHAIN=/actual/sdk/out/toolchain/nds32le-linux-musl-v5d \
  -DSPEEX_ROOT=/actual/riscv32/speexdsp-install \
  -DALSA_INCLUDE_DIR=/actual/riscv32/alsa-install/include \
  -DALSA_LIBRARY=/actual/riscv32/alsa-install/lib/libasound.a \
  -DAEC_WITH_ALSA=ON -DBUILD_TESTING=OFF
cmake --build build/aec-v821 -j
```

以上静态库配置已在恢复的 SDK 上完成构建，并经Mac USB/ADB部署到板上独立目录，
`--help` 运行退出0。若改用共享库，则库需在板上存在且 ABI 相符；工具链 sysroot
未必包含 SDK staging 的 alsa-lib。独立工具的一轮短时间连续采播已通过，长期、声学和双讲验证仍需继续。

板上 mixer 沿用 [board_config.h](../../bsp/boards/v821_aitoy/board_config.h) 的已知
配方，尤其确认 `rx sync mode=Off`。核对控件后再设置，并保持各轮硬件增益一致；
不要在这轮混入 MPP 的同步配置。

准备不超过 15 秒的语音 `speech-16k.raw`，在板上手动运行：

```sh
./v821_aec_probe record --device hw:0,0 --play speech-16k.raw \
  --prefix /mnt/UDISK/aec-run01 --seconds 20 --gain-pct 25
```

`--gain-pct` 是软件增益，不代表实际声压。每轮换 prefix；20 秒三路 PCM 约 1.92MB，
另有 CSV，先确认剩余空间。Ctrl-C 停止声卡、保留部分数据并返回非零。退出码 0
只证明本轮没有报告 I/O 中断，不证明声学效果合格。

输出为 `.mic.raw` / `.ref.raw` / `.out.raw` / `.csv`。CSV 的 capture/playback delay
是在各自调用时取得的驱动排队样本数，不是统一时刻的声学路径延时。

## 板上验收顺序

1. **近端单讲**：软件播放增益为 0，人说话，听原始麦克风和输出，检查削波、
   底噪、音色及实时处理预算。
2. **远端单讲**：人保持安静，播放语音。从录音测参考到 mic 的延时，比较首尾
   窗口，看是否漂移。只在无近端讲话的窗口统计回声衰减，并结合底噪评估。
3. **双讲**：播报期间人说话，确认人声、首字和尾字保留。覆盖远近距离、几档
   音量，以及播放结束后立即说话。
4. **负载**：检查 CSV 耗时、排队深度和 XRUN；先排除落盘阻塞，再加入 UI、
   相机、人形检测。Mac 耗时不能代替 V821 的 10ms 帧预算。
5. **产品打断**：声学验证通过后，本地确认插话就立即停止本地播放、清队列，
   并请求云端 ChatBreak，不能等云端确认后才停喇叭。持续采集和 AEC，保留短
   预录防止丢首字；隔离旧回答迟到的音频及 START/END，验证多轮和 MCP。

可把“插话开始到喇叭停止不超过 300ms”作为第一版候选目标，但必须分别记录
用户发声、本地判定、声卡停止、云端断句时间，不能拿 ChatBreak 成功代替听感验收。

先用 SpeexDSP 建立可测基线；若效果或 CPU 预算不足，再在同一组录音上比较
厂商 WebRTC AEC / ECNR。64KB 是旧管道的容量估算，不是实测固定延时；旧笔记对
`MAX_DELAY=100` 的单位推断也需 SDK 原件复核，不能当成确定的 100ms 算法上限。

参考：[ALSA PCM](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html)、
[Speex AEC](https://www.speex.org/docs/manual/speex-manual/node7.html)。
