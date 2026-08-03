# 代码结构设计

> 目标：把原先平铺的 `src/` 分层，让「平台相关的坑」和「业务逻辑」分开，
> 方便后续扩展（视频方案、换音频后端、换板子）。
>
> **状态：已实施并真机验证**（对话 + 识图正常）。下文「现状诊断」一节描述的是
> 重构前的状态，保留下来说明为什么这么改。
> 未验证项：扫码配网（设备当时无法解绑，只用 `--demo-dev` 跑的）。

---

## 1. 现状诊断：乱在哪

```
src/
├── v821_chatbot.c      704 行  ← 音频I/O + VAD + AEC + 状态机 + TAI回调 + mixer配方 全在一起
├── v821_voice_chat.c   855 行  ← 早期验证程序，已废弃，但还在构建里
├── cam_v4l2.c          368 行
├── ui_fb.c             312 行
├── tai_boot.c          274 行
├── mcp_tools.c         198 行
├── prov.c              151 行
├── lv_conf.h           759 行  ← 配置文件混在源码根
├── fonts/ emoji/               ← 资源混在源码根
└── demo_reconnect.h            ← 从 SDK 例程拷来的，其实没用上
dist/ dist2/ dist3/             ← 三个历史遗留目录，前两个已无用
```

四个具体问题：

1. **`v821_chatbot.c` 一个文件五种职责**。改 VAD 阈值要在 700 行里找，加个 MCP 工具
   也在同一个文件。今天调 AEC 时反复在这个文件里翻，很痛。
2. **平台相关和业务逻辑没分开**。V4L2 的 MPLANE、fbdev 的 `FBIOPAN_DISPLAY`、
   mixer 的 numid 配方、arecord/aplay 管道 —— 这些全是这块板/这个内核特有的，
   而对话状态机、VAD 判定、JSON 解码跟平台无关。混在一起的后果是：
   将来把音频 I/O 换成进程内 ALSA（或 MPP AO/AI 做硬件 AEC），要动主程序。
3. **死代码在构建里**：`v821_voice_chat.c` 855 行 + `demo_reconnect.h`。
4. **`dist/` `dist2/` `dist3/`** 三个目录，只有 `dist3/` 还在用。

---

## 2. 参考对象

### xiaozhi-esp32（C++）

```
main/
├── application.cc, device_state_machine.cc, mcp_server.cc, ota.cc, settings.cc
├── boards/common/board.h    ← 虚接口：GetAudioCodec/GetDisplay/GetCamera/GetNetwork/GetLed...
│   └── <每块板一个目录>       ← DECLARE_BOARD 宏 + create_board() 工厂 + 单例
├── audio/  display/  led/  protocols/  assets/
```

### xiaozhi Linux 版（九鼎 SDK 里的 chatbox）

```
core/     平台无关：application, protocol, transport, websocket, http, ota, storage, iot
core/posix/   OS 抽象：task, mutex, queue, event_group, sys_time
port/     平台相关：audio_codec, audio_process, display, mac, opus_wraper, port_network
```

### ★ 你们自己的 ESP 仓库（agentic-kit-solution）

**已经是 xiaozhi board 抽象的 C 版**（`components/bsp/include/bsp/board.h`）：

```c
struct bsp {
    const char *name;
    uint32_t    caps;            /* BSP_CAP_DISPLAY / AUDIO / CAMERA / TOUCH */
    esp_err_t (*init)(void);
    esp_err_t (*audio_start)(int sample_rate);
    int       (*audio_read)(int16_t *mono, int frames);
    ...
};
const bsp_t *bsp_get(void);
#define DECLARE_BOARD(BSP_PTR) const bsp_t *bsp_get(void) { return (BSP_PTR); }
```

它的设计文档定了四层：**产品层 `solutions/` → 服务层 `ak_*` → 板级层 `bsp/` → SDK**，
并明说「用 C 函数指针表（与 pal_t 一致），不引入 C++」。

**结论：对齐团队既有约定 = 同时对齐 xiaozhi，不存在二选一。**
而且他们设计里规划了 `ak_ai_session` / `ak_ui_chat` 两个服务组件但一直没做
（"session setup and UI still live in each product's main.c"）—— 我们这里正好可以
先把它们做出来，将来两边合并时是现成的。

---

## 3. 提议的结构

```
quanzhi/
├── CMakeLists.txt
├── toolchain-v821.cmake
├── README.md
├── docs/architecture.md            ← 本文档
│
├── bsp/                            【板级层】硬件细节，一板一目录
│   ├── include/bsp/board.h         平台中立的 vtable（见 §4）
│   └── boards/v821_aitoy/
│       ├── board.c                 组装 vtable + DECLARE_BOARD
│       ├── board_config.h          设备节点 / mixer numid / 屏幕尺寸
│       ├── audio_alsa.c            arecord/aplay 管道 + mixer 配方
│       ├── display_fb.c            /dev/fb0 + FBIOPAN_DISPLAY
│       └── camera_v4l2.c           V4L2 MPLANE + S_PARM
│
├── core/                           【核心层】平台无关的业务逻辑
│   ├── app.c/h                     对话状态机（聆听 → 上送 → 播放）
│   ├── vad.c/h                     能量 VAD + 开机自动标定
│   ├── aec.c/h                     speexdsp 封装
│   ├── ringbuf.c/h                 播放 ring
│   └── jsonx.c/h                   最小 JSON + 两层 \uXXXX 解码
│
├── cloud/                          【服务层】涂鸦云对接
│   ├── session.c/h                 连云引导 + TAI 回调 + token 解析
│   ├── provision.c/h               扫码配网 + 凭据持久化
│   └── mcp.c/h                     MCP 工具注册与分发（take_photo…）
│
├── ui/                             【界面层】
│   ├── ui.c/h                      对话界面（LVGL widgets）
│   ├── ui_qr.c                     配网二维码（直画 fb，不走 LVGL）
│   ├── lv_conf.h  lv_drv_conf.h
│   └── assets/{fonts,emoji}/
│
├── app/main.c                      骨架：板级init → 配网 → 连云 → 起状态机
├── scripts/                        板上脚本（合并原 dist/dist2/dist3）
│   ├── run_chatbot.sh
│   └── xstar                       开机自启包装
├── tools/gen_emoji_16bpp.py
└── third_party/                    构建时准备，gitignore
```

对应关系：`bsp/` ↔ ESP 的 `components/bsp`；`cloud/` ↔ 他们规划的 `ak_ai_session`
+ 已有的 `ak_provisioning`；`ui/` ↔ 他们规划的 `ak_ui_chat`；`core/`+`app/` ↔
`solutions/<product>/main`。

---

## 4. 板级 vtable：必须平台中立

ESP 那份 `bsp_t` 的公共头**泄漏了平台类型**（`esp_err_t`、`i2c_master_bus_handle_t`），
他们的 CLAUDE.md 自己标注了「第二个平台一进来这个抽象就漏了」。
**我们就是那第二个平台**，所以这里定义自己的中立类型：

```c
typedef int bsp_err_t;                  /* 0 = OK，负数 = 错误 */

#define BSP_CAP_DISPLAY (1u << 0)
#define BSP_CAP_AUDIO   (1u << 1)
#define BSP_CAP_CAMERA  (1u << 2)

typedef struct bsp {
    const char *name;                   /* "v821_aitoy" */
    uint32_t    caps;

    bsp_err_t (*init)(void);            /* 上电、设 mixer 配方 */

    /* 音频：产品侧只见 16bit 单声道 PCM，管道/ALSA/MPP 的差异藏在实现里 */
    bsp_err_t (*audio_start)(int sample_rate);
    int       (*audio_read)(int16_t *mono, int frames);   /* 返回帧数 */
    int       (*audio_write)(const int16_t *mono, int frames);
    void      (*audio_mic_enable)(int on);   /* 一问一答要物理关麦 */
    void      (*audio_drain)(void);          /* 等播放真正放完（pclose 语义） */
    void      (*audio_stop)(void);

    /* 显示：产品侧只给 RGB565 缓冲和尺寸，谁去 pan 是板子的事 */
    bsp_err_t (*display_start)(int *w, int *h);
    void      (*display_flush)(const void *rgb565, int x, int y, int w, int h);

    /* 相机：产品侧只要一张 JPEG */
    bsp_err_t (*camera_start)(int w, int h);
    int       (*camera_capture_jpeg)(const uint8_t **jpg, size_t *len,
                                     int *w, int *h);
    void      (*camera_stop)(void);
} bsp_t;

const bsp_t *bsp_get(void);
#define DECLARE_BOARD(P) const bsp_t *bsp_get(void) { return (P); }
```

这样换音频后端（管道 → 进程内 ALSA → MPP AO/AI 做硬件 AEC）只改
`boards/v821_aitoy/audio_*.c`，`core/` 和 `app/` 一行不动。

---

## 5. 评估：哪些值得做，哪些是过度设计

### 值得做

| 项 | 理由 |
|---|---|
| **拆 `v821_chatbot.c`** | 五种职责在一个文件里，改任何一处都要在 700 行里定位。这是当前最痛的。 |
| **bsp 层隔离平台代码** | 不是为了"多块板"，而是**这个项目里平台坑占了大半调试时间**（V4L2 MPLANE、S_PARM、fbdev 不自动 pan、mixer 配方、管道缓冲语义）。把它们围起来，业务逻辑就干净了。而且硬件 AEC 那条路一定要换音频后端，有这层就只改一个文件。 |
| **删死代码** | `v821_voice_chat.c` 855 行 + `demo_reconnect.h` + `dist/` `dist2/`。 |
| **资源和配置挪出源码根** | `lv_conf.h`(759行) / `fonts/` / `emoji/` 挤在 `src/` 里。 |

### 不值得做（明确不做）

| 项 | 为什么不做 |
|---|---|
| **OS 抽象层**（仿 xiaozhi 的 `core/posix/`） | 我们只跑 Linux，pthread/socket 直接用。xiaozhi 那层是为了同时支持 FreeRTOS。 |
| **协议抽象**（仿 xiaozhi 的 `protocols/`） | 我们只有涂鸦 TAI 一种，抽象出来没有第二个实现去验证它。 |
| **多板目录**（现在只有一块板） | 目录结构留好（`boards/v821_aitoy/`），但不为假想的第二块板做任何额外抽象。 |
| **C++ / 面向对象** | 团队约定就是 C 函数指针表，且 agentic-kit 内核是 C99。 |

### 风险

约 2000 行**已验证可用**的代码要搬家，有搬坏的风险。缓解办法：

- 以**纯文件移动 + 最小改动**为主，不趁机重写逻辑
- 分 3 步提交，每步都能编译：
  ① 删死代码 + 目录搬家（不改逻辑）
  ② 抽 bsp 层（音频/显示/相机三个实现挪进去，主程序改调 `bsp_get()->xxx()`）
  ③ 拆 `v821_chatbot.c` → `core/` + `cloud/` + `app/main.c`
- **每步之后真机冒烟测一次**（对话一轮 + 拍照一次），确认没搬坏

---

## 6. 实施结果

按上述设计一次做完，编译零警告，真机验证对话 + 识图正常。

与设计稿的两处差异：

- 服务层目录定名 **`service/`**（设计稿里写的是 `cloud/`）。它对应 ESP 仓库
  `components/ak_*` 那一层；不逐个拆成 `ak_session/` `ak_provisioning/` 是因为
  那种拆法源于 ESP-IDF「一个组件一个目录」的硬要求，我们是普通 CMake 工程，
  一个目录三个文件更清爽。
- `core/vad.c` + `core/aec.c` 合并成 **`core/audio_dsp.c`**。两者在实现上强耦合：
  标定必须走与判定完全相同的 AEC+降噪管道（原始 mic 底噪约 4700，降噪后只剩
  约 500，差 10 倍，混用尺度会让阈值完全失效——这是踩过的坑）。硬拆成两个文件
  只会让这个约束更容易被破坏。

数字对比：

| | 重构前 | 重构后 |
|---|---|---|
| 最大单文件 | `v821_chatbot.c` 704 行，5 种职责混在一起 | `core/app.c` 约 200 行，只有状态机 |
| 死代码 | `v821_voice_chat.c` 855 行 + `demo_reconnect.h` | 已删 |
| 目录 | `src/` 平铺 + `dist/` `dist2/` `dist3/` | 5 层 + `scripts/` |
| 产物 | 2.3M，只依赖 libc.so | 不变 |
