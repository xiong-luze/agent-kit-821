# agent-kit-821 —— 涂鸦 AI 语音方案在九鼎 V821 上的移植

把 [tuya/agentic-kit](https://github.com/tuya/agentic-kit) 移植到**九鼎 V821 开发板**（全志 V821，
Tina Linux），验证并实现设备通过涂鸦云做 AI 语音对话，并在上面做了一个完整的
AI 语音 + 视觉设备（对话上屏 / 拍照识图 / 看到有人主动打招呼）。

> **这是个人的移植与验证工作，不是涂鸦或九鼎的官方项目**，两家都不为它提供支持。
> 上游 SDK 请以 [tuya/agentic-kit](https://github.com/tuya/agentic-kit) 为准。
>
> 板级细节（V4L2 / fbdev / ALSA / mixer 配方）只在这一块板上验证过。换板子见
> §2.5 的分层说明，`bsp/boards/` 那一层就是为此准备的。
>
> 用到的第三方素材（字体、表情）的来源与许可见
> [`ui/assets/NOTICE.md`](ui/assets/NOTICE.md)。

**文档**：分层设计与取舍见 [`docs/architecture.md`](docs/architecture.md)；
官方开发包（3.2G）里哪个文件回答哪个问题，见
[`docs/vendor-sdk-notes.md`](docs/vendor-sdk-notes.md)。

---

## 0. 已支持的功能

「已验证」= 在真机上跑通并观察到预期行为，不是"代码写完了"。

### ✅ 已验证

| 功能 | 说明 | 细节 |
|---|---|---|
| **连涂鸦云** | 走 agentic-kit 的 `tuya_ai`(tRTC-over-TLS) + `iot_client`；断线自动重启恢复 | §6 |
| **多轮语音对话** | 一问一答形态。本地能量 VAD 起止 + 云端 ASR eof 双判据，谁先到算谁 | §5、§8 |
| **防自问自答** | 播放期间**物理关闭 arecord**，不依赖 AEC；靠 `pclose(aplay)` 等真正播完 | §8 |
| **对话文字上屏** | LVGL + `/dev/fb0`，中文字体 + 流式追加 | §5.5 |
| **情绪表情** | 云端 `SKILL/emo` → 右上角 emoji 图；文本里的 `\uXXXX` 残留会剥掉 | §5.5 |
| **MCP 视觉问答** | 注册 `take_photo`，大模型自己决定何时拍照，图走 TAI 图像通道上传 | §5.6 |
| **主动打招呼** | 待机 60s 后本地人形检测确认有人靠近 → 设备自己开口（本地视觉唤醒） | §5.7 |
| **VAD 自动标定** | 开机静听量底噪定阈值，阈值会抬到盖住底噪峰值 | §5 |
| **凭据持久化** | `/mnt/UDISK/tuya_creds.txt`，掉电重启不用重新配网 | §5.4 |

### ⚠️ 有条件可用

| 功能 | 限制 |
|---|---|
| **扫码配网** | 二维码能出、流程跑通，但**「扫码→激活→存盘」未端到端验证**（设备当时无法解绑）。当前用已有凭据运行 |
| **软件 AEC** | speexdsp 代码在（`--no-aec` 可关），但**全双工打断没做成** —— 管道架构拿不到时间对齐的参考信号，量化理由见 §8 |
| **吵闹环境的语音触发** | 能量 VAD 分不出「你说话」和「旁边同事说话」，这是唤醒词才能解决的问题。办公室实测会被持续人声触发一堆空对话。用 `--greet-only` 可只留主动打招呼 |

### ❌ 未支持 / 阻塞中

| 功能 | 阻塞原因 |
|---|---|
| **本地语音唤醒** | 官方 KWS 库（`libuvoicekws.so`）是 **ARM** 的，V821(riscv32) 加载不了。要么找九鼎要 riscv32 版，要么自己上 sherpa-onnx —— 但人形检测已占 31% CPU，得先算账 |
| **硬件 AEC / 全双工** | 需要走 MPP AO/AI，而 MPP 全静态链接、官方固件没编进来 → 要全量重编 + 刷机。可用的替代是"进程内 ALSA + 官方预编译 `libAec.a`"，不用刷机 |
| **BLE 配网** | SDK 有协议层，但本板固件把 BT 用户态整个裁掉了（无 `hciattach`/`bluetoothd`） |
| **视频通话** | 涂鸦 `rtc-client` 的 `libstm.a` 没有 riscv32 版。板子的 H.264 硬编和 MPP sample 都在，等涂鸦给库 |
| **TLS 证书校验** | 当前全程 `peer verification disabled`。**量产硬阻塞项**，下一步优先做 |

**这颗芯片没有 NPU**（datasheet 零命中，定位是 IPC SoC）。人形检测是全志官方预编译的
riscv32 库跑**纯 CPU 软推理**，单帧 167ms @320×192。详见
[`docs/vendor-sdk-notes.md`](docs/vendor-sdk-notes.md) §4.5。

---

## 1. 板子的确切身份

| 项 | 值 | 影响 |
|---|---|---|
| SoC | 全志 V821（驱动代号 **sun300iw1**），双 RISC-V：主核 1GHz + MCU 600MHz(AMP) | IPC 定位，带 ISP/H264/JPEG 硬编解码 |
| 架构 | **riscv32**，`rv32imafdc` + `xandes5p0`，**ilp32d** ABI | 必须用 SDK 自带工具链 |
| libc | **musl**（`/lib/ld-musl-riscv32.so.1`） | 不是 glibc |
| 系统 | Tina Linux 5.0 = OpenWrt 21.02-SNAPSHOT，target `v821-aitoy`，kernel **5.4.220** | |
| 内存 | 64MB DDR2（片内 SiP），**用户可见 43.8MB，可用约 25MB** | 主要约束 |
| 存储 | SPI NAND 256MB（W25N02K）；`/mnt/UDISK` = ubi0_9，23MB 可写 | 部署放这里 |
| Wi-Fi | 内置 2.4G 802.11b/g/n（XRADIO），用全志自己的 `wifi` 命令 | 无独立 wpa_supplicant |
| 音频 | 片内 codec，1×DAC + 1×ADC，MICINP/N 差分 | 增益偏低，见 §5 |
| **无 RTC** | 冷启动时间回 1970 | **TLS 必挂，开机必须先对时** |

## 2. 依赖与环境

```
~/kit/agentic-kit          # 涂鸦 SDK（公开仓，需 git submodule update --init --recursive --depth 1）
~/kit/v821-tina-v12        # 九鼎 SDK（从 DVD_X821/source/v821.tar.gz 解出，已排除 openwrt/dl）
~/kit/quanzhi              # 本仓库
```

> **九鼎 SDK（`v821-tina-v12`）需要你自己从九鼎获取**（开发板附带的光盘 /
> 他们的下载渠道）。本仓库不包含也不转发它的任何内容 —— 工具链、LVGL、
> 人形检测库都是从那份 SDK 里取的，下面每一处都写了取自哪个路径。
> 官方包的导读见 [`docs/vendor-sdk-notes.md`](docs/vendor-sdk-notes.md)。

宿主机要求：

```sh
pip3 install --user cmake            # 系统 cmake 3.16 太老，SDK 要 >=3.20
pip3 install --user jsonschema jinja2 # mbedTLS 3.6 构建期用 python 生成源码
export PATH=~/.local/bin:$PATH
```

工具链（SDK 自带，先解压一次）：

```sh
mkdir -p ~/kit/v821-tina-v12/out/toolchain
tar -xJf ~/kit/v821-tina-v12/prebuilt/rootfsbuilt/riscv/nds32le-linux-musl-v5d.tar.xz \
        -C ~/kit/v821-tina-v12/out/toolchain/
# → riscv32-linux-musl-gcc 10.4.0，默认 -march/-mabi 就与板子固件逐字段一致，无需额外参数
```

speexdsp（主目标 `v821_chatbot` 的 AEC 依赖，**必需**）：

```sh
tar -xzf <v821.tar.gz 里的 openwrt/dl/speexdsp-1.2.0.tar.gz> -C third_party/
cd third_party/speexdsp-1.2.0 && PATH=<toolchain>/bin:$PATH ./configure \
  --host=riscv32-linux-musl --prefix=$PWD/../speexdsp-install \
  --disable-shared --enable-static --disable-examples \
  CC=riscv32-linux-musl-gcc CFLAGS="-O2" && make -j && make install
```

libjpeg-turbo（相机 JPEG 软编码，**必需**）。注意 cmake 4.x 要加
`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`，rv32 无 SIMD 要 `-DWITH_SIMD=0`：

```sh
tar -xzf <v821.tar.gz 里的 openwrt/dl/libjpeg-turbo-2.1.1.tar.gz> -C third_party/
cd third_party/libjpeg-turbo-2.1.1 && cmake -S . -B build-rv32 \
  -DCMAKE_TOOLCHAIN_FILE=<写一个指向 riscv32-linux-musl-gcc 的 toolchain 文件> \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_INSTALL_PREFIX=$PWD/../jpeg-install \
  -DWITH_SIMD=0 -DENABLE_SHARED=0 -DENABLE_STATIC=1 -DWITH_TURBOJPEG=1 \
  && cmake --build build-rv32 -j && cmake --install build-rv32
```

LVGL 8.3 + fbdev 驱动（屏幕显示，**必需**）——直接从九鼎 SDK 拷：

```sh
cp -r ~/kit/v821-tina-v12/platform/thirdparty/gui/lvgl-8/lvgl third_party/lvgl/lvgl
# 注：不用 lv_drivers 的 fbdev.c，原因见 §5.5；字体和 emoji 见 ui/assets/、tools/
```

人形检测库（「看到有人主动打招呼」用，**可选**）——全志官方预编译的
riscv32+musl 静态库，直接从九鼎 SDK 拷：

```sh
V=~/kit/v821-tina-v12/openwrt/package/allwinner/vision/person_detection/src
mkdir -p third_party/pdet/lib third_party/pdet/include
cp $V/lib/musl/*.a $V/3rdparty/lib/musl/libaw_utilities_standalone_v821.a third_party/pdet/lib/
cp $V/include/*.h $V/3rdparty/include/*.h third_party/pdet/include/
```

缺了不影响构建，只是主动打招呼功能关闭（CMake 会打印一行 STATUS）。
它是 C++ 实现，CMake 会用 g++ 链并 `-static-libstdc++`，所以板上不需要额外的
运行时库。V821 **没有 NPU**，这是纯 CPU 软推理，细节见
[`docs/vendor-sdk-notes.md`](docs/vendor-sdk-notes.md) §4.5。

opus（只有 WAV/Opus 那两个验证 target 用；主目标上下行都走 PCM，不需要）：

```sh
tar -xzf <v821.tar.gz 里的 openwrt/dl/opus-1.3.1.tar.gz> -C third_party/
cd third_party/opus-1.3.1 && PATH=<toolchain>/bin:$PATH ./configure \
  --host=riscv32-linux-musl --prefix=$PWD/../opus-install \
  --disable-shared --enable-static --disable-doc --disable-extra-programs \
  CC=riscv32-linux-musl-gcc && make -j && make install
```

## 2.9 密钥：设备三元组

涂鸦三元组（`TUYA_UUID` / `TUYA_AUTH_KEY` / `TUYA_PRODUCT_KEY`）**绝不写进源文件**：

```sh
cp tuya_authkey.txt.example tuya_authkey.txt   # 已 gitignore
$EDITOR tuya_authkey.txt                       # 填涂鸦 IoT 平台给的真实值
chmod 600 tuya_authkey.txt
```

构建时 CMake 解析它，生成 `build/generated/tuya_authkey.h`，`app/main.c` 只 include
那个生成的头。缺文件是 `FATAL_ERROR`，会直接告诉你该怎么做。挂了
`CMAKE_CONFIGURE_DEPENDS`，改完 txt 不用手动 reconfigure。

只提交 `tuya_authkey.txt.example`。**三元组只用于首次扫码配网**；激活后拿到的
`devid`/`secret_key`/`local_key` 存在板上 `/mnt/UDISK/tuya_creds.txt`（0600），
之后就不再需要三元组了。

`--demo-dev`（跳过配网、直接用一台已激活设备）的凭据同样放在这个文件里，是**可选**的
三个键 `DEMO_DEVID` / `DEMO_SECRET_KEY` / `DEMO_LOCAL_KEY`。三个都填了这个开关才可用，
没填就只是开关不可用、其余照常。等价做法是直接传位置参数：

```sh
./v821_chatbot <devid> <secret_key> <local_key>
```

> **解析上的一个坑**：`secret_key` 是随机可打印字符，可能含分号。CMake 把分号当列表
> 分隔符，所以这里**没有**用 `file(STRINGS)` + `foreach` 按行遍历（那样含分号的行会被
> 劈成两段、那个键**静默**解析不到），而是整体 `file(READ)` 后逐个键对全文做正则。
> ESP 仓库那份解析用的是 foreach 写法，它们的 key 恰好都是字母数字所以没暴露。

## 2.5 代码结构

分层设计与取舍见 [`docs/architecture.md`](docs/architecture.md)。形态取自 xiaozhi 的
Board 抽象（`DECLARE_BOARD` + 函数指针表），并与团队 ESP 仓库
（agentic-kit-solution 的 `components/bsp`）保持一致。

```
app/main.c        产品层：板级init → 屏幕 → 音频 → 凭据/配网 → 连云 → MCP → 状态机
bsp/              板级层：平台相关的坑全关在这里，一板一目录
  include/bsp/board.h      平台中立的 vtable（不泄漏任何平台类型）
  boards/v821_aitoy/       board.c / board_config.h（mixer 配方）/
                           audio_alsa.c / display_fb.c / camera_v4l2.c
core/             平台无关：app.c(对话状态机) audio_dsp.c(VAD+AEC)
                  presence.c(有人靠近检测) ringbuf.c text.c jsonx.c
service/          涂鸦云（≈ ESP 仓库的 ak_*）：session.c 连云+TAI回调 /
                  provision.c 扫码配网 / mcp.c 设备工具
ui/               界面：ui.c 对话 / ui_qr.c 二维码 / lv_conf.h / assets/{fonts,emoji}
scripts/          板上脚本：run_chatbot.sh / xstar(开机自启)
tools/            gen_emoji_16bpp.py
```

换板子 = 新增一个 `bsp/boards/<板名>/` 填一张 `bsp_t`，`-DBOARD=<板名>` 选中，
上层一行不改。换音频后端（管道 → 进程内 ALSA → MPP AO/AI 做硬件 AEC）
只改 `bsp/boards/v821_aitoy/audio_*.c`。

## 3. 构建

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-v821.cmake
cmake --build build -j
```

产物只依赖 `libc.so`（mbedTLS/coreMQTT/coreHTTP/cJSON、LVGL、speexdsp、libjpeg-turbo、
人形检测库全静态链接，含 `-static-libstdc++`），strip 后 **3.2MB**。

| target | 说明 |
|---|---|
| `v821_chatbot` | **主目标**：见 §0 功能清单 |
| `activate_demo` | 用三元组 + 配网 token 激活（拿 devid/secret_key/local_key） |
| `tai_text_chat_demo` | 纯文本对话，最小连云验证 |

## 4. 部署与运行

```sh
tar -cf v821_chatbot.tar -C dist .         # 板子的 busybox tar 没有 -z，不要压缩
adb push v821_chatbot.tar /mnt/UDISK/
adb shell "cd /mnt/UDISK && tar -xf v821_chatbot.tar && chmod +x *.sh v821_chatbot && ./run_chatbot.sh"
```

`run_chatbot.sh` 会自动处理三个前置条件：

1. **停官方应用**：`/usr/share/xstar/xstar` 占着声卡，且 `/usr/bin/daemon.sh` 每 30 秒把它拉起来 →
   必须先 `kill $(pgrep -f daemon.sh)` 再 `killall xstar`
2. **对时**：`ntpd -q -n -p ntp.aliyun.com`（无 RTC，不对时 TLS 必挂）
3. **确认 wlan0 有 IP**（`wifi -c "<SSID>" "<PASS>"` 连网）

热替换口子：`/etc/init.d/S99xstar` 会优先执行 `/mnt/UDISK/xstar`，所以正式应用放那个路径即可
免刷固件迭代。注意 `daemon.sh` 的复活逻辑写死的是 `/usr/share/xstar/xstar`，替换时要一起处理。

## 5. 音频通路配方（重要）

**板级 dts 在音频上基本没调过**：`mic-gain=<31>` / `lineout-gain=<31>` 超出这颗芯片驱动的 mask
（0xF / 0x7）被截断；`rx-sync-en` 会让普通 `arecord` 直接失败。以下每项都是实测必需的，
程序启动时会自动设一遍（`amixer cset` 不持久，重启即丢）：

| numid | 控件 | 板级默认 | 设成 | 为什么 |
|---|---|---|---|---|
| 2 | `rx sync mode` | On | **0 Off** | 置 `RX_SYNC_EN` 后 ADC DMA 等外部同步信号，`arecord` 报 `pcm_read: I/O error`。**代价：失去官方硬件 AEC** |
| 13 | `ADC Volume` | 160 (+0.75dB) | **200** (+30dB) | 麦克风电平只有 -44dBFS。注意 `amixer sset` 找不到此控件（access 带 volatile），**必须 `cset numid=`** |
| 14 | `MIC Gain` | 15 | 15 | 已是模拟 PGA 上限（15dB） |
| 6 / 8 | `ADC HPF0/1` | Off | **1 On** | 不开有约 -1500 的直流偏置，+30dB 后是明显嗡声（症状：采样值全为负） |
| 4 | `DAC HPF` | Off | **1 On** | 播放侧去直流 |
| 15 | `LINEOUT Gain` | 3 (-36dB) | **5** (-24dB) | 每档 6dB |
| 19 | `Soft Volume Master` | 130 (0dB) | 130 | 细调，每档 0.2dB |

驱动：`bsp/drivers/sound/platform/snd_sun300iw1_codec.c`
板级：`device/config/chips/v821/configs/aitoy/linux-5.4-ansc/board.dts` 的 `&codec`

**信噪比偏差**：底噪 RMS≈2050，说话 RMS 2700~7900（SNR 约 12dB）。模拟增益不足只能靠数字补，
底噪一起放大。ASR 实测能准确识别，但量产音质建议让九鼎复核麦克风电路。
因此 VAD 阈值**开机自动标定**，不写死。三个坑：
① 标定必须走**与判定完全相同的管道**（AEC+降噪）—— 原始 mic 底噪约 4700，
经 speex 降噪后只剩约 500，差 10 倍，混用会让阈值完全失效；
② 取**低四分位**而不是均值，并丢掉前 0.5 秒等降噪收敛 —— 标定期间一声杂音就
能把均值拉高好几倍，阈值跟着虚高、之后再也触发不了；
③ **但抬完还必须盖住实测峰值**。踩过：`floor=155 → start=155×2.5=387`，而底噪
尖峰是 `511` —— 环境噪声自己就不停触发上送，每轮 ASR 返回空文本，既不出声，
还把 §5.7 的主动打招呼彻底堵死（每次假对话都在推迟待机计时）。现在阈值取
`max(低四分位×2.5, 峰值×1.2)`，并封顶 8 倍底噪（防一声门响把阈值拽到听不见人说话）。

## 5.4 配网激活（涂鸦 App 扫码）

`service/provision.c`，对应 SDK 的 `examples/posix/pair/scan-by-app`。

```
开机 → 读 /mnt/UDISK/tuya_creds.txt
  ├─ 有 → 直接连云
  └─ 无 → iot_get_qrcode_info() 取激活 URL → 二维码显示在屏上+终端
          → iot_client_init_on_boarding() 阻塞等 App 扫（180s）
          → devid/secret_key/local_key 存盘(0600)
```

- **这是"设备已联网"的绑定激活**：板子必须先连上 WiFi，二维码只负责把设备绑到
  用户的涂鸦账号。App 下发 WiFi 密码那种配网需要 BLE 或 SoftAP，见 §9。
- **`iot_init_default()` 必须在任何 `iot_*` 调用之前**，包括
  `iot_get_qrcode_info()`。它只是设 PAL 和 cJSON hooks，重复调用安全。
- **二维码直接画 framebuffer，不走 LVGL**：二维码必须一个模块对齐整数个像素，
  走 LVGL 的图片缩放会糊边、手机扫不出来。按
  `min(屏宽,屏高)/(模块数+静默区)` 取整数倍放大。
- **qrcodegen 用 LVGL 自带的那份**（`lvgl/src/extra/libs/qrcode/`），别再 vendor
  一份 nayuki 的——符号会撞（multiple definition）。
- 开关：`--reprov` 清存档重新配网，`--demo-dev` 用 SDK 例程的测试设备跳过配网。

## 5.5 屏幕显示（LVGL + /dev/fb0）

屏是 **SPI/DBI 的 ST7789S 320×240**（板级 dts `spi_panel_spi1`，`lcd_pixel_fmt=0x0a`
= RGB565），内核 `bsp/drivers/video/sunxi/lcd_fb` 把它注册成标准 framebuffer。

- **必须自己调 `FBIOPAN_DISPLAY`**。SPI 屏不像 RGB 并口屏由控制器自动扫描，
  内核驱动里唯一的推屏入口就是 `fb_pan_display`（内部 `set_layer` + `wait_for_vsync`），
  而且**没有后台刷新线程**。`lv_drivers/display/fbdev.c` 只往 mmap 内存里写、
  从不 pan（源码里那句 `FBIO_UPDATE` 是注释掉的）—— 用它画面永远不出现。
  所以 `bsp/boards/v821_aitoy/display_fb.c` 自己实现了 flush：
  逐行拷进 mmap + `ioctl(FBIOPAN_DISPLAY)`；`ui/ui.c` 只管交出 RGB565 缓冲。
- 整屏缓冲 + `full_refresh=1`：SPI 屏每次推屏都是整帧，分块刷没意义。
- **UI 单独一条线程**（30ms 一刷）。外部只调 setter 写字符串缓冲（加锁），
  音频主循环一行 LVGL 都不碰。
- 中文字体用官方 chatbox 现成的阿里普惠体 14px/1bpp（`ui/assets/fonts/`）。
- 情绪用 21 张 32×32 图（右上角）。官方素材是**只按 32bpp 生成**的
  （4096B/张，无 `LV_COLOR_DEPTH` 分支），本机是 16bpp，必须转换 ——
  见 `tools/gen_emoji_16bpp.py`。按**情绪名**（SKILL 包的 `emotion[0]`）映射，
  不按码点：官方那 21 张的语义注释正好是同一套词表。
  对话文字里的 emoji 一律剥离（puhui 没有这些字形，留着是豆腐块）。

## 5.6 摄像头（V4L2）与 MCP 视觉问答

GC2083 MIPI sensor，内核 `sunxi-vin` 驱动，`/dev/video0`。

- **是 MPLANE 驱动**（`vin_video.c` 注册的是 `vidioc_enum_fmt_vid_cap_mplane`）：
  buf type 必须 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`、格式走 `fmt.pix_mp`、
  buffer 带 `m.planes[]`。用单平面 API 的话 `ENUM_FMT` 直接返回空。
- **必须先 `VIDIOC_S_PARM` 且 `capturemode = V4L2_MODE_VIDEO`(0x0002)**，
  再 `S_FMT`。顺序取自官方样例 `sample_driverVipp.c`：
  `S_INPUT → S_PARM → S_FMT → REQBUFS → QBUF → STREAMON`。
  漏掉 S_PARM 的现象是 **STREAMON 返回成功、但 DQBUF 永远超时**。
- **`S_FMT` 成功不代表出得了流**：驱动只查静态格式表（`vin_core.c` 的
  `vin_formats[]`，52 项），不校验 ISP/sensor 实际能力。比如 RGB24 会被欣然
  接受却一帧都不出。所以 `camera_v4l2.c` 对每个候选格式**真的抓一帧来验证**，
  失败就 teardown 换下一个。候选顺序 `NV21 → NV12 → YUV420 → YVU420 → RGB24`
  （NV21 是官方样例默认，这条路最通）。
- 格式表里**没有 MJPEG**，JPEG 只能软编码（libjpeg-turbo）。V821 有硬件 JPEG
  编码器但要走 MPP，而官方固件没编 MPP 音视频。
- ISP **不需要**单独启动：官方样例里 `ISP_RUN` 是 `#if 0`。
- 抓帧前丢 4 帧等 AE/AWB 收敛，否则常拿到过曝或全黑的图。

**MCP（大模型主动调用设备工具）**：云端把 JSON-RPC 2.0 请求作为 `TAI_EVT_MCP_CMD`
推来，要答三个方法 `initialize` / `tools/list` / `tools/call`（格式抄 SDK 的
`examples/posix/ai/rtc-tcp-client/mcp_demo.c`）。我们注册了 `take_photo`。

- **图片不能走 MCP 返回值** —— MCP 工具结果只能是文本
  （`content:[{type:"text"}]`）。所以拍照后走 TAI 图像通道
  `tai_send_image_with_text()` 上传，MCP 只回一句"已拍照"。
- **拍照必须在独立线程**：MCP 回调跑在 TAI 接收线程上，而 V4L2 取帧 + 软件
  JPEG 编码在 rv32 上要几百毫秒。`tools/call` 只置标志立刻返回。
- 这版 SDK **没有** `tai_set_event_params()`（那是 ESP 侧较新 SDK 的 API），
  只有 `tai_config_t` 里静态的 `session_attrs_json` / `event_user_data_json`。
  走 MCP 时本来就是大模型主动要图，不需要像 ESP 侧那样切 `clm_intent` 工作流。

## 5.7 看到有人主动打招呼（本地视觉唤醒）

待机久了没人说话，摄像头确认有人靠近就自己开口 —— 这是**本地语音唤醒的替代**
（官方那两个唤醒词库是 ARM 的，V821 加载不了，见 `docs/vendor-sdk-notes.md` §4）。

分工是「本地小模型做门控，云端大模型做理解」：本地只判断「有没有人」，
`core/presence.c` 用全志官方预编译的 riscv32 人形检测库（纯 CPU，V821 没 NPU）。

### ★★ 判据是窗口命中率，不是「连续 N 帧」★★

最早一版喂的是最近邻缩放 + 拉伸到 5:3 的图，实测**人站着不动 35 帧只检到 13 帧
（37%）**，结果 `1,1,0,0,1,1,0,...` 乱跳 —— 「连续 3 秒都检到」这个条件几乎
永远不成立。改成面积平均 + letterbox（见下）之后实测：

| 场景 | 命中率 |
|---|---|
| 人在镜头前 | **~100%**（连续 18 帧滑动窗口稳定 100%） |
| 空场景 | **~9%**，而且都是**孤立单帧**误检 |

判据仍然保留命中率形式而不是回到「连续 N 帧」—— 真人会动、会侧身、光线会变，
不能假设单帧永远可靠。门限取 **60%**：有人时轻松过；空场景要在 18 帧里凑出
11 个误检，统计上不可能。

**两级采样**，兼顾 CPU 和判断可靠性：

```
巡视 WATCH：每 1 秒抓一帧。CPU ≈ 167/1000 = 17%
   │  任意一帧检到人（宁可误入，让下一级去否掉）
   ▼
确认 CONFIRM：连着抓帧（不留间隔）3 秒 ≈ 18 帧，统计命中率
   │  命中率 ≥ confirm_ratio，且距上次打招呼 ≥ 3 分钟
   ▼  → 置 trigger，转 FIRED
静默 FIRED（刚打过 / 人还在）
   │  连续没人累计 ≥ 30 秒
   ▼  → 回 WATCH
```

确认期**连着抓帧**是故意的：3 秒能取约 18 个样本，统计上才分得开「有人时的
37%」和「空场景的误检率」；每秒一帧的话 3 秒只有 3 个样本，判不出来。代价是那
3 秒 CPU 满载，但确认期很短、且只在待机时发生。

状态机是**边缘触发**：人坐在镜头前不动只会被打一次招呼，
**必须「人离开再回来」才重新触发**。另外从关到开会强制重新武装
（`presence_set_enabled(1)` 置 `s_rearm`）—— 刚聊完的人就站在镜头前，
不该等 60 秒后又被搭话一次。

计时一律用**时间**而不是帧数：单帧耗时随画面复杂度变，用帧数会让「3 秒」
变成一个漂移的值。

### 喂给检测器的图要处理干净（这是召回低的原因之一）

- **面积平均缩放**，不是最近邻。640×480 → 320×192 是 2×2.5 倍下采样，
  最近邻直接丢掉 60% 的行，人的边缘被采样噪声打碎。多花几毫秒，
  相比一帧 167ms 可忽略。
- **letterbox 保宽高比**。相机 4:3 直接压到模型的 5:3 会把人竖着挤扁 1.25 倍，
  对人形检测器是实打实的分布偏移。默认等比缩放 + 居中灰边（YOLO 惯用的 114）。
  `V821_PDET_FIT=stretch` 可退回拉伸做对照。

**招呼语由云端生成**，不是本地写死的字符串：走 `session_send_text()` 发一句指令
给云端，云端照常出 NLG + TTS。刻意不提性别 —— 本地检测只知道「有个人」
（`AW_Det_Person` 就 `bbox`/`label`/`prob` 三个字段），猜错了比不猜更糟。

### ★ CPU 抢占是这个功能最大的风险

检测是纯 CPU 软推理，和音频主循环抢同一个 1GHz 标量核，而音频那边是 20ms 硬
节拍 —— 被抢了就掉帧、AEC 失效、云端断流（这类坑踩过好几次，见 §8）。所以：

- **只在 `ST_LISTEN` 且待机够久时开检测**，一进对话（`ST_SEND`/`ST_WAIT`）立刻
  `presence_set_enabled(0)`，线程只睡不干活
- 每跑完一帧睡满 `interval_ms`（默认 500ms）把核让出去
- 取帧走 `bsp_t.camera_capture_rgb()`：不编码 JPEG、不丢帧预热，直接缩到
  320×192 拷给调用方。和 MCP 拍照共用同一个 `/dev/video0`，靠
  `camera_v4l2.c` 里的 mutex 串行化（两个线程各自 DQBUF 会乱序拿 buffer）

- 线程自己 `nice +10`，让 20ms 硬节拍的音频线程随时抢得过检测

### 调参

```sh
./v821_chatbot --pdet-tune       # 打印单帧耗时 + 累计/近18帧命中率
```

**要量两个场景**，`confirm_ratio` 取两者中间：

| 场景 | 量到的是 |
|---|---|
| 人站在镜头前不动 | 「有人」的命中率（实测 letterbox 前是 37%） |
| 空场景没人 | 误检率 |

已实测：**连着跑**时单帧稳定 167ms，但头几帧是 ~310ms。
**稀疏采样时会一直停在慢档**：巡视期一秒一帧的情况下，确认期 3 秒只取到
**9 帧**（→ 单帧约 330ms），不是按 167ms 算的 18 帧。看着是 CPU 调频没爬上来
（330/167 ≈ 2）。两个后果：

- CPU 占用按 **31%** 估，不是 17%
- `CONFIRM_MIN_SAMPLES`（现为 5）不能定太高，否则确认期永远"样本不足"

端到端实测一次成功的时序：

```
确认期 9 帧命中 8（89%，门限 60%）     ← 人走过来
[招呼] 已请云端生成招呼语              07:17:12
SKILL/emo → HAPPY                     07:17:14
首个 TTS 音频包                        07:17:15
```

即「确认通过 → 出声」约 3 秒，加上确认期本身 3 秒，**从人出现到设备开口约 6 秒**。
偏慢——人可能已经走过去或坐下了。想快可以把 `V821_PDET_CONFIRM_MS` 降到 2000
（约 6 帧，仍高于最小样本数 5），能省下约 1 秒。

| 环境变量 | 默认 | 作用 |
|---|---|---|
| `V821_GREET` | 1 | 0 = 关掉整个功能（等同 `--no-greet`） |
| `V821_GREET_IDLE_MS` | 60000 | 待机多久才开始看有没有人 |
| `V821_PDET_WATCH_MS` | 1000 | 巡视期采样周期 |
| `V821_PDET_CONFIRM_MS` | 3000 | 确认期时长 |
| `V821_PDET_RATIO` | 60 | 确认门限，百分比（实测有人100%/空场景9%，取中间） |
| `V821_PDET_TH` | 30 | 检测器置信度门限，百分比 |
| `V821_PDET_REARM_MS` | 30000 | 走开多久重新武装 |
| `V821_PDET_GAP_MS` | 180000 | 两次招呼最小间隔 |
| `V821_PDET_FIT` | letterbox | `stretch` = 退回拉伸做对照 |
| `V821_VOICE_TRIGGER` | 1 | 0 = 关掉声音触发，只留主动打招呼（等同 `--greet-only`） |

### 吵闹环境下怎么验证（`--greet-only`）

**能量 VAD 分不出「你说话」和「旁边同事说话」** —— 这是唤醒词才能解决的问题，
而本板没有可用的本地唤醒（§4 的官方库是 ARM 的）。实测在办公室：标定那 2 秒
刚好安静（`底噪 88，区间 58~408`，阈值抬到 487，抬高逻辑工作正常），但持续的
人声超过 487，日志里是 `[上送] 3.6 秒（本地静音）` —— 连续 3 秒多都在阈值之上，
是真有人在说话，不是尖峰。**再调阈值也分不出来。**

所以要在吵闹环境里单独验证主动打招呼，把声音触发关掉：

```sh
V821_GREET_IDLE_MS=15000 ./v821_chatbot --greet-only
```

这不是正常形态，只是测试开关。正常环境下别用。

## 6. 涂鸦云契约要点

- **TTS 必须请求 PCM，不要 Opus**：`sessionAttributes.tts.order.supports[].format = "pcm"`。
  云端起播会把 N 个等长 Opus 帧**聚合成一个大 payload**（实测 2560B = 32×80B），
  单次 `opus_decode` 直接 `OPUS_INVALID_PACKET`，开头整段丢失。改 PCM 后首音时延也从 3.9s 降到 1.2s。
- 上行用 `TAI_AUDIO_PCM`（16k/16bit/mono）即可，与 ESP 侧量产方案一致。
- `chatAttributes`：`{"sys.workflow":"asr-llm-tts","asr.enableVad":true,"tts.alternate":true,"processing.interrupt":true}`
- 文本包：`bizType=ASR` 取 `data.text`（用户话），`NLG` 取 `data.content`（回答，分句流式，
  带 `timeIndex`=该句音频时长，可做口型/表情同步），`SKILL`+`code=emo` 取
  `data.skillContent.emotion[0]`（如 `HAPPY`）→ 可直接映射 `/usr/share/xstar/assets/eyes/<小写>/*.mpg`
- `TAI_EVT_MCP_CMD` **必须回** `tai_send_mcp_response`（长连接下不回会卡）
- **一次回答可能跨多个 TAI 事件**。MCP 拍照就是两个：先回"稍等我看看"（事件 A），
  图片上传后再回识别结果（事件 B）。所以**不能拿单个事件的 END 判定整体播完** ——
  A 的 END 到达时 ring 恰好抽空就会误判结束，B 的音频整段丢掉（屏幕文字却是全的，
  因为文字走另一条路径）。判"播完"要三个条件同时成立：ring 空 + `g_turn_done` +
  **距上次收到音频超过 800ms**；另外 `TAI_EVT_START` 和音频的 `stream_flag=START`
  都要清掉 `g_turn_done`。
- **上送结束别只靠本地 VAD**。云端给了两个更可靠的信号：ASR 的 `eof=1`（云端 VAD
  判定说完）、以及 TTS 音频开始到达（云端早就往下走了）。只用本地能量 VAD 的话，
  它一旦没判出来就会硬撑到 `MAX_TURN_MS`（15 秒），而云端其实几秒前就出了 ASR、
  音频也到了 —— 表现就是"回答很慢"，但慢在上送侧不在播放侧。
- **文本要过两层解码**，否则屏幕上是 `\u0020` / `\u0022` 这类码：
  ① `json_get_string_esc()` 解 JSON 转义（`\uXXXX`、代理对、内嵌 `\"`——
  朴素提取器遇到内嵌引号还会提前截断）；
  ② `json_unescape_u_inplace()` 再解**字面量**残留 —— 云端做了双重编码，
  `\\u0020` 解完一层 JSON 得到的是字面的 6 个字符 `\u0020`，不是转义。
- ⚠️ **TLS 目前完全不校验证书**（`No CA certificate provided`），量产必须带涂鸦 CA
- 现在用的是 SDK 例程里**涂鸦自己的测试设备**凭据（人设"安琪"）。要配自己的智能体，
  需用自己的三元组走 `activate_demo`（还需要一个配网 token：OpenAPI 或 App 扫码）

## 7. 视频方案的阻塞项

agentic-kit 有两条并行的路：

| | `rtc-tcp-client`（`tai_*`，在用） | `rtc-client`（`libstm.a`） |
|---|---|---|
| 交付 | 全源码 | **预编译 .a，无源码** |
| 平台 | 任意（C99 + 15 个 PAL 函数） | 仅 mips / rockchip-arm / aarch64 / amd64 / macOS-arm64 |
| 文本/音频/**静态图片** | ✅ | ✅ |
| **视频流** | ❌ | ✅ |
| riscv32 | ✅ 已跑通 | ❌ **没有这个架构的构建产物** |

**语音对话和拍照识别都不需要 `libstm.a`**。只有做视频流（摄像头上云、视频通话、大模型看实时视频）
才必须找涂鸦补一个 **riscv32-musl** 的构建产物。

## 8. 回声消除 / 防自问自答

**当前形态是一问一答：播放期间物理关闭 arecord**（`pclose(rec)`），麦克风根本不在
采集，AI 的声音不可能被录回去。这不依赖 AEC、不依赖任何时序假设或阈值，是这个
需求下最可靠的解法。两个必须注意的点：

- **等"播完"只能靠 `pclose(play)`**。它会阻塞到 aplay 把管道和内部缓冲全部放完、
  进程退出。不能靠 `fwrite` 返回判断 —— 我们和 aplay 之间有 64KB 的 OS 管道缓冲
  （约 2 秒音频），"写完"比"播完"早最多 2 秒，那期间重开麦就会把尾音录进去。
- 之后再等 `V821_GUARD_MS`（默认 300ms）覆盖房间混响，才重开 arecord。
- 播放 ring 要够大（40 秒）：云端会在 6 秒内把 12 秒以上的 TTS 突发推完，
  装不下就被丢弃，回答会播到一半没了。

speexdsp 软件 AEC 的代码仍保留（`--no-aec` 可关），用于消掉播放期间的回声，
但**全双工打断没有做成**：试过多轮，arecord/aplay 管道这套架构把音频时钟交给了
两个外部进程和管道缓冲，任何调度抖动都会破坏 ref/mic 的逐帧对齐，而 AEC 恰恰
要求这个对齐严格成立。要做全双工建议直接上官方硬件 AEC（见下）或改成进程内 ALSA。

**核心设计：播放由采集时钟驱动。** `on_audio` 只把 TTS 放进 ring，主循环每 20ms 取一帧，
**同一帧数据既喂 `aplay` 又喂 AEC 的 ref 输入**。这样 ref 和 mic 由同一个时钟推进，天然对齐，
只差一个固定小延迟（aplay 100ms 缓冲 + 声学 + 录音缓冲），落在滤波器覆盖范围内 —— 这是
自适应滤波器能收敛的前提。

反过来做（收到 TTS 就往 `aplay` 灌）会有两个致命问题，都踩过：
1. 网络突发时管道能积压 2 秒，ref/mic 相对延迟不可控，AEC 必然失效；
2. 积压的音频在播放结束后被读到、被 VAD 当成用户输入 → **自问自答**。
   教训：音频程序必须**永远消费输入流**，只按状态决定这帧丢弃还是上送。

参数：帧 320 样本(20ms)，滤波器 4800 样本(300ms，`V821_AEC_TAIL` 可调)，
speex 降噪开、**AGC 关**（mixer 已有 +30dB，再叠自动增益会把底噪和 AEC 残留一起顶上来；
全志 AEC 文档 §4.6 也警告增益过大会让 AEC 变差）。
打断阈值取 VAD 起始阈值的 1.5 倍、需连续 120ms，避免 AEC 残留误触发。

调试：`--aec-dump` 把 in/ref/out 三路落盘到 `/mnt/UDISK/aec_*.raw`，用 Audacity 三轨对齐看
（对应全志文档 §4.1.2 的 `AI_HW_AEC_DEBUG_EN` 做法）。`--no-aec` 一键退回半双工做对照。

### 为什么没走官方硬件 AEC（后续仍要做）

SDK 文档 `~/kit/v821-tina-v12/docs/pdf/board/MPP/Tina_Linux_MPP_AEC回声消除_调试指南.pdf`
的方案是「硬件同步 (txhub + rxsync)」，
AEC 被做成 MPP 的 AI 模块开关（`AIOAttr.ai_aec_en = 1`，不用自己喂 ref）。但：

- **官方 aitoy 固件没有编进 MPP 音频** —— `/usr/lib/eyesee-mpp/` 只有 `librgb_ctrl.so`，
  全盘 grep `WebRtcAec|ai_aec_en|AW_MPI_AI` 零命中；连官方 `xstar` 自己都是链 `libasound` 走 ALSA 的。
  要用就得 `menuconfig` 开 `eyesee-mpp → enable aec` → **全量重编 + PhoenixSuit 重刷固件**，
  并把音频 I/O 从 arecord/aplay 改成 `AW_MPI_AO/AI`。
- 它依赖 `rx sync mode = On`，与我们当前让 `arecord` 能工作的配置冲突（见 §5）。
- 前置成本：需要把解压时排除的 3GB `openwrt/dl` 解回来，OpenWrt 全量编译占 15~25G 磁盘。

**硬件条件的隐忧**（文档自己的警告正好命中本板）：§4.6 "MIC 采集 Gain 大容易引起 AEC 效果差"、
§4.5 "播放声音过大容易引起 AEC 效果差"、§2.2 要求 mic 与 speaker 拉开距离 + 隔音棉 + 密闭音腔。
本板麦克风电平只有 -44dBFS 必须靠 +30dB 数字增益补，且裸板无任何声学结构。
所以硬件 AEC 的最终效果要等产品结构件出来才能真正评估。

## 9. 其他已知限制 / 下一步

### BLE 配网：SDK 支持，但本板固件缺件

`agentic-kit/modules/tuya-ble/`（745 行）只是**协议层** —— 广播数据生成、加密配对
握手、解析 App 下发的 SSID/密码/token。它不含 BLE 协议栈，需要外部提供
`send_fn`（GATT notify）、`tuya_ble_prov_on_data()`（喂 GATT write）、
以及用 `tuya_ble_prov_get_adv_data()` 去设置广播。ESP 侧的胶水是
`ak_provisioning/tuya_ble_nimble.c`（NimBLE，360 行），V821 要写 BlueZ 版。

**但官方 aitoy 固件把 BT 用户态整个裁掉了**（和 MPP 音频一样）：

| | 状态 |
|---|---|
| BT 芯片 / 内核驱动 | ✅ XRADIO combo，dmesg 里 `xrbt_smd` 探测正常 |
| `hciattach` | ❌ 不存在。`/etc/bluetooth/bt_init.sh` 明确依赖它（`hciattach -n ttyS1 xradio`）才能起 `hci0` |
| `bluetoothd` | ❌ 不存在，没有 BlueZ 的 D-Bus GATT 外设接口 |
| `libbtmg.so` | ⚠️ 文件在，但**导出符号数为 0**，是空壳 |

所以要做 BLE 配网：① 重编固件开 bluez-utils + 刷机；② 验证 XRADIO 的 BT
支不支持 LE peripheral 角色（未知，建议先问九鼎）；③ 写 BlueZ GATT 外设胶水
约 400 行；④ 联调 App。①和硬件 AEC 是同一件事（都要重编+刷机），值得合并做。

备选是 SoftAP 配网：板子的 `wifi -o ap` 现成，但 **agentic-kit 没有 AP 配网模块**，
设备端的 HTTP 那段要自己写。
- `xiaozhi/port/audio_process.c`（官方 chatbox 的音频处理）**是空壳**，6 个函数全无实现；
  `word_detect.h` 只有头文件无 `.c` —— 唤醒词和 AEC 在那份源码里都没有。
- 官方包里还有哪些能用/不能用（含一个可直接链的 riscv32 `libAec.a`、
  ARM 唤醒词库是死路、128 份 SDK PDF 的索引），见
  [`docs/vendor-sdk-notes.md`](docs/vendor-sdk-notes.md)。
- 官方带源码的可复用件在 `platform/allwinner/`：
  `llm/chatbox/xiaozhi`（73 c/cpp，ALSA/LVGL/表情字体/协议分层）、
  `multimedia/rt_media/demo/demo_audio_alsa.c`、`multimedia/rt_media-mpp_demo/src/demo_*`。
  `usr/share/xstar/xstar` 本身**只有二进制没源码**，不能改。
