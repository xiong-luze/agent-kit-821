# V821 全双工 AEC 与本地唤醒

本开发版提供持续采音、回声消除、本地语音打断和可配置唤醒词，默认短语为“你好涂鸦”。
在一块V821测试板上完成了基本多轮对话、打断后继续回答，以及受控声卡欠载后的恢复测试。
默认构建仍保留原管道音频后端与半双工入口，使用新链路需要显式选择配置。

这不是量产声学验收结果。当前紧凑模型的声学样本集为10/11，仍有一个音色正例漏检；
远场、多人、噪声、长期误唤醒率、实际断网恢复及端到端打断时延需要在目标硬件上继续验证。

## 音频与会话流程

- `bsp/boards/v821_aitoy/audio_pcm.c`通过进程内ALSA持续采播，16kHz单声道S16_LE，
  20ms处理帧，请求60ms声卡缓冲，并检查实际协商结果。
- 实际送入播放声卡的PCM作为AEC参考。可选SpeexDSP或厂商SDK中的WebRTC静态库；
  后者使用10ms分块、moderate NLP，并保留Speex降噪，关闭AGC。
- 待唤醒时原始麦克风输入只供本地关键词检测；预录、上送和插话检测使用AEC输出。
  唤醒后默认保留30秒连续对话窗口，再回到待唤醒。
- 插话确认后先清本地播放缓冲，再排入云端打断和新一轮输入。默认预录600ms，
  采音线程不等待网络发送或关键词推理。
- 发送队列固定160槽，预算为两秒发送阻塞、最多一秒预录及控制余量；溢出明确报错，
  不静默丢帧。已知旧事件ID被隔离，新回答需要有效的新ASR事件后才放行。
- 声卡XRUN/挂起会使当前采播连续性失效。此时停止整个采播、关键词和发送链路，
  回收MCP及连接，复位DSP并重新启动；不会只恢复一条声卡流后继续使用旧AEC参考。
  连续快速失败最多重建三次，稳定运行30秒后恢复预算。
- 关闭SDK上下文前调用`tai_disconnect()`停止并等待接收线程退出，再释放上下文。
  网络故障按1/2/4/5秒间隔重试；其他设备或模型错误明确终止。

参考延时由处理帧和实际播放排队深度估算，不能等同于实测的声学延时。全双工模式暂停
原CPU人形主动招呼，MCP拍照入口保留；其与插话的组合场景仍需实板验证。

## 依赖与构建

依照[主README](../README.md)准备SDK、LVGL、JPEG等原有依赖。新链路还需要目标架构的
alsa-lib、SpeexDSP和PocketSphinx 5.0.4。V821依赖必须为riscv32、ilp32d、musl，不能使用
主机、ARM或aarch64的库。厂商SDK和预编译AEC库由开发者自行获取，不随本仓库分发。

在独立的公开上游agentic-kit副本应用
[`patches/agentic-kit-late-control-event-id.patch`](../patches/agentic-kit-late-control-event-id.patch)。
它修复晚到的旧ChatBreak/END覆盖新会话缺省事件ID的问题；兼容基线和回归命令见
[`patches/README.md`](../patches/README.md)。然后把CMake的`AK`指向该副本。

下面路径为示例，请替换为自己的工具链及目标依赖安装目录：

```sh
cmake -S . -B build/v821-voice \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/toolchain-v821.cmake" \
  -DV821_TOOLCHAIN="$HOME/sdk/toolchain/nds32le-linux-musl-v5d" \
  -DAK="$HOME/kit/agentic-kit-voice" \
  -DSPEEX_ROOT="$HOME/deps-rv32/speexdsp" \
  -DALSA_INCLUDE_DIR="$HOME/deps-rv32/alsa/include" \
  -DALSA_LIBRARY="$HOME/deps-rv32/alsa/lib/libasound.a" \
  -DPOCKETSPHINX_ROOT="$HOME/deps-rv32/pocketsphinx" \
  -DV821_AUDIO_BACKEND=alsa -DV821_AEC_BACKEND=webrtc \
  -DWEBRTC_AEC_ROOT="$HOME/sdk/aec_lib/webrtc" \
  -DV821_ENABLE_WAKE_WORD=ON
cmake --build build/v821-voice -j
```

`WEBRTC_AEC_ROOT`须包含`include/aec_lib.h`和`library/musl/libAec.a`。
若选择Speex，改为`-DV821_AEC_BACKEND=speex`，其声学参数需要单独验证。
设备凭据仍只放在本地被忽略的`tuya_authkey.txt`；用示例文件作为填写模板。
生成的凭据头和固件二进制不应作为公开源码附件上传。

## 唤醒模型与启动

模型准备工具及上游资源来源见[唤醒工具说明](../tools/wake/README.md)。例如：

```sh
python3 tools/wake/prepare_model.py \
  --source build/wake-deps/cmusphinx-zh-cn-5.2 \
  --output build/wake-deps/mandarin-kws-tuya --phrase 你好涂鸦
```

更换短语时必须重新生成匹配的模型和词典，再同时更新短语、模型目录与词典路径。
只改显示文字不会更新模型。完整应用的短语上限为96个UTF-8字节。模型及其派生参数、词典、许可证应放在同一部署目录；
原始资源与用户录音均不随源码提交。

部署布局可以是：

```text
/mnt/UDISK/agent-kit-821/
  v821_chatbot
  run_chatbot.sh
  run_chatbot_duplex.sh
  voice_duplex.env
  wake/tuya/              # 派生模型、wake.dict、manifest及许可文件
```

将`scripts/run_chatbot*.sh`、可执行程序及模型复制到目标目录。SDK-WebRTC测试板配置可以
从`scripts/voice_duplex.webrtc.env.example`复制成`voice_duplex.env`；模型路径采用相对于
启动脚本所在目录的路径。该配方的ADC增益和VAD门槛只在当前板型初测，换硬件要重新标定。

```sh
cd /mnt/UDISK/agent-kit-821
sh ./run_chatbot_duplex.sh --no-greet
```

`--demo-dev`仅在开发者本地配置了自己的测试设备凭据时可用；仓库没有可直接使用的设备
身份。此启动入口不会修改开机配置。`--no-wake`可单独排查AEC，`--half-duplex`保留回退；
全双工与`--no-aec`组合会报错。

## 配置及调参边界

| 配置 | 含义 |
|---|---|
| `V821_WAKE_WORD` / `V821_WAKE_MODEL` / `V821_WAKE_DICTIONARY` | 匹配的关键词、模型和词典 |
| `V821_WAKE_THRESHOLD` | KWS阈值；代码保守默认`1e-25`，测试板候选`1e-35` |
| `V821_WAKE_MIN_RMS` | 原始输入活动资格门槛，仍须真实KWS命中；测试板为350 |
| `V821_VAD_START` / `V821_VAD_END` | AEC输出的起说话/结束门槛；WebRTC测试板为500/300 |
| `V821_BARGE_MS` / `V821_PRE_ROLL_MS` | 插话确认时间和上送预录；测试板为120/600ms |
| `V821_WAKE_HOLD_MS` | 连续对话窗口，默认30000ms |
| `V821_VOICE_TRACE=1` | 数值时序、队列和状态诊断，按需开启 |

小声或远场输入可能被活动门槛拒绝。放宽阈值也可能增加误唤醒；应在正例、近似词、
环境噪声和长时间待机样本上一起评估，不把一次命中或一个人的测试当作总体准确率。
长时间低能量输入期间会维护初始CMN特征基线，降低已复现的背景状态漂移影响。

## 验证范围

主机检查可在不提供设备凭据的情况下编译实际控制器、会话及产品入口，并通过替身驱动
网络、声卡和相机边界。依赖与外部音频不同，注册的测试数量也会不同。例如：

```sh
cmake -S . -B build/voice-host \
  -DV821_BUILD_HOST_TESTS=ON -DV821_TEST_UBSAN=ON \
  -DAK="$HOME/kit/agentic-kit-voice" \
  -DSPEEX_ROOT="$HOME/deps-host/speexdsp" \
  -DPOCKETSPHINX_ROOT="$HOME/deps-host/pocketsphinx" \
  -DALSA_TEST_INCLUDE="$HOME/deps-host/alsa/include"
cmake --build build/voice-host -j
ctest --test-dir build/voice-host --output-on-failure
```

Apple Silicon若使用Rosetta启动的CMake，应与依赖一起显式选择`arm64`。
可选模型、回放和UI头文件检查的参数见`tests/CMakeLists.txt`；声学测试使用开发者自行
准备且有权使用的外部音频，路径保留在忽略目录中。独立SDK事件分派回归见
[`tests/sdk/README.md`](../tests/sdk/README.md)。

当前已有的验证包括：开发环境18组主机回归、V821主程序和辅助目标交叉构建、基本真人
多轮对话及打断，以及一次实板受控欠载后的完整恢复，后者约8.8秒。已知未完成项包括
远场/多人/噪声声学质量、端到端打断时延、自然长测、实际硬断网恢复及拍照与插话竞争。
紧凑唤醒模型的声学质量门仍为10/11，不能作为量产识别率。

## 发布范围

源码仓库包含实现、配置模板、测试及工具。真实设备凭据、内网访问令牌、个人主机配置、
原始会话/录音日志、厂商预编译库和模型产物应留在本地；提交前检查暂存文件及差异。
基础项目和素材许可说明继续见[LICENSE](../LICENSE)及
[ui/assets/NOTICE.md](../ui/assets/NOTICE.md)。
