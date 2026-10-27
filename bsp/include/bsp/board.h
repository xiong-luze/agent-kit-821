/*
 * bsp/board.h —— 板级抽象（Board Support Package）
 *
 * 一块板由一张函数指针表 bsp_t 描述。上层（core/ service/ ui/ app/）只调
 * bsp_get()->xxx()，永远不碰设备节点、ioctl、mixer numid。换板子 = 新增一个
 * boards/<板名>/ 填一张表，上层一行不改。
 *
 * 这套 vtable + DECLARE_BOARD 的形态取自 xiaozhi 的 Board 抽象，用 C 函数指针
 * 表而不是 C++ 虚函数 —— 与团队 ESP 仓库（agentic-kit-solution 的
 * components/bsp）以及 agentic-kit SDK 的 pal_t 保持一致。
 *
 * ★ 与 ESP 那版的区别：这里的类型是**平台中立**的。ESP 那份公共头泄漏了
 * esp_err_t 和 i2c_master_bus_handle_t，他们自己标注了"第二个平台一进来这个
 * 抽象就漏了"——V821 就是那第二个平台，所以这里只用 int / 标准类型。
 */
#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include <stdint.h>
#include <stddef.h>

/* 0 = OK，负数 = 错误 */
typedef int bsp_err_t;
#define BSP_OK 0
/* Sample continuity was lost (e.g. XRUN/suspend). Close and restart BOTH
 * streams and reset AEC/reference state; never silently resume one stream. */
#define BSP_ERR_AUDIO_DISCONTINUITY (-1001)

/* 板子有哪些外设（bsp_t.caps 位掩码）。NULL 的表项 = 该板没有这个外设。 */
#define BSP_CAP_AUDIO   (1u << 0)
#define BSP_CAP_DISPLAY (1u << 1)
#define BSP_CAP_CAMERA  (1u << 2)
#define BSP_CAP_AUDIO_DUPLEX (1u << 3)

typedef struct bsp {
    const char *name;        /* 板名，如 "v821_aitoy" */
    uint32_t    caps;        /* BSP_CAP_* */

    /* 上电 / 板级初始化（本板：设音频通路 mixer 配方）。必须最先调。 */
    bsp_err_t (*init)(void);

    /* ---- 音频 ----------------------------------------------------------
     * 上层只见 16bit 单声道 PCM。底下是 arecord/aplay 管道、进程内 ALSA、
     * 还是 MPP AO/AI，上层不关心。 */
    bsp_err_t (*audio_start)(int sample_rate);
    /* 阻塞读一帧，返回实际帧数（<=0 表示流断了）。麦克风关闭时返回 0。 */
    int       (*audio_read)(int16_t *mono, int frames);
    int       (*audio_write)(const int16_t *mono, int frames);
    /* 物理开关麦克风。一问一答要在播放期间真正关掉采集，
     * 这样 AI 的声音不可能被录回去（不依赖 AEC）。 */
    void      (*audio_mic_set)(int on);
    /* 阻塞到已写出的音频**真正播完**（不是"写完"）。
     * 播放侧和上层之间通常隔着缓冲，写完远早于播完；这个接口负责抹平差异。 */
    void      (*audio_drain)(void);
    void      (*audio_stop)(void);
    /* Duplex-only: stop queued playback immediately, preserving capture.
     * pending includes silence as well as speech; both return negative on error. */
    int       (*audio_flush)(void);
    int       (*audio_pending)(void);

    /* ---- 显示 ----------------------------------------------------------
     * 上层给 RGB565 缓冲，怎么送到屏上（自动扫描 / 手动 pan）是板子的事。 */
    bsp_err_t (*display_start)(int *w, int *h);
    void      (*display_flush)(const void *rgb565, int x, int y, int w, int h);
    void      (*display_stop)(void);

    /* ---- 相机 ----------------------------------------------------------
     * 上层只要一张 JPEG。取帧格式协商、色彩转换、软/硬编码都在板子里。
     * jpg 由板子持有，下次调用前有效。 */
    bsp_err_t (*camera_start)(int want_w, int want_h);
    int       (*camera_capture_jpeg)(const uint8_t **jpg, size_t *len,
                                     int *w, int *h);
    /* 抓一帧，面积平均缩放成 dst_w×dst_h 的 RGB24 **拷贝**进 dst
     * （需 dst_w*dst_h*3 字节）。给本地视觉推理用：不编码 JPEG、不做 AE 预热
     * 丢帧，比拍照便宜得多。只做缩放，**不管宽高比** —— 要不要 letterbox 是
     * 视觉侧的判断，不该由板级决定。
     * 拷贝而不是返回内部指针，是为了能和 camera_capture_jpeg 在不同线程并发调用
     * ——两者内部互斥，返回后调用方持有的数据不会被另一个线程改写。 */
    int       (*camera_capture_rgb)(uint8_t *dst, int dst_w, int dst_h);
    /* 协商到的取帧分辨率。上层要靠它算宽高比。 */
    void      (*camera_get_size)(int *w, int *h);
    void      (*camera_stop)(void);
} bsp_t;

/* 编译期选定的那块板。 */
const bsp_t *bsp_get(void);

/* 具体板子用它注册自己。一次构建只编进一块板。 */
#define DECLARE_BOARD(BSP_PTR) \
    const bsp_t *bsp_get(void) { return (BSP_PTR); }

#endif /* BSP_BOARD_H */
