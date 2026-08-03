/*
 * board_config.h —— 九鼎 V821 aitoy 板的硬件常量
 *
 * 这里的每个数字都是实测确定的，改之前先读 README 的 §5（音频通路配方）
 * 和 §5.5/§5.6（屏幕、相机）。
 */
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ---- 音频 ---------------------------------------------------------------
 * 走 arecord/aplay 管道。这两个工具在本板验证可用，且不需要 alsa-lib。
 * aplay 用小缓冲：缓冲越大，播放到麦克风的回声延迟越大。 */
#define BOARD_ALSA_CAPTURE_DEV  "hw:0,0"
#define BOARD_AREC_CMD \
    "arecord -q -D " BOARD_ALSA_CAPTURE_DEV " -t raw -f S16_LE -r 16000 -c 1 2>/dev/null"
#define BOARD_APLAY_CMD \
    "aplay -q -t raw -f S16_LE -r 16000 -c 1 --buffer-time=100000 --period-time=20000"

/* 板级 dts 在音频上基本没调过（mic-gain/lineout-gain 写的值超出这颗芯片驱动的
 * mask 被截断；rx-sync-en 会让 arecord 直接 I/O error）。以下每项都是实测必需，
 * 且 amixer cset 不持久，每次启动都要重设。详见 README §5。 */
#define BOARD_MIXER_SETUP { \
    "amixer -c 0 cset numid=2  0   >/dev/null 2>&1",  /* rx sync mode Off，否则录音 I/O error */ \
    "amixer -c 0 cset numid=13 200 >/dev/null 2>&1",  /* ADC Volume  +30dB，板子麦克风电平低 */ \
    "amixer -c 0 cset numid=14 15  >/dev/null 2>&1",  /* MIC Gain    模拟 PGA 上限 */ \
    "amixer -c 0 cset numid=6  1   >/dev/null 2>&1",  /* ADC HPF0 On，去直流偏置 */ \
    "amixer -c 0 cset numid=8  1   >/dev/null 2>&1",  /* ADC HPF1 On */ \
    "amixer -c 0 cset numid=4  1   >/dev/null 2>&1",  /* DAC HPF  On */ \
    "amixer -c 0 cset numid=15 5   >/dev/null 2>&1",  /* LINEOUT Gain -24dB */ \
    "amixer -c 0 cset numid=19 130 >/dev/null 2>&1",  /* Soft Volume 0dB */ \
    NULL }

/* ---- 显示 ---------------------------------------------------------------
 * SPI/DBI 的 ST7789S，内核 lcd_fb 驱动注册成标准 framebuffer，RGB565。 */
#define BOARD_FB_DEV  "/dev/fb0"

/* ---- 相机 --------------------------------------------------------------- */
#define BOARD_CAM_DEV "/dev/video0"

#endif /* BOARD_CONFIG_H */
