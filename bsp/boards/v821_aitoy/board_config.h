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
/* 启动时的探测：喂一帧(20ms)静音，只看退出码。popen 察觉不到 aplay 打不开声卡，
 * 没有这一步就会变成「有文字没声音」且无任何报错。见 audio_alsa.c。 */
#define BOARD_APLAY_PROBE \
    "head -c 640 /dev/zero | " \
    "aplay -q -t raw -f S16_LE -r 16000 -c 1 >/dev/null 2>&1"

/* 板级 dts 在音频上基本没调过（mic-gain/lineout-gain 写的值超出这颗芯片驱动的
 * mask 被截断；rx-sync-en 会让 arecord 直接 I/O error）。以下每项都是实测必需，
 * 且 amixer cset 不持久，每次启动都要重设。详见 README §5。 */
/* 一条 mixer 设定。desc 会在**设置失败时**打出来 —— 原来这些是拼好的命令串、
 * 后面跟着 >/dev/null 2>&1，返回值也没人看，于是失败完全静默。实际就踩了：
 * numid=19 在这套固件上根本不存在（amixer: Cannot find the given element），
 * 而它想设的值恰好等于默认值，所以一直没人发现。 */
typedef struct {
    int         numid;
    int         value;
    const char *desc;      /* NULL = 表结束 */
} board_mixer_t;

/* LINEOUT Gain(numid=15) 的实测刻度：0~1 = mute，2 = -42dB，之后每档 +6dB，
 * 所以 5 = -24dB、4 = -30dB、7 = -12dB（上限）。 */
/* The full-duplex SDK AEC profile needs capture headroom: ADC200 clipped
 * speaker-only input on this board; ADC180 removed clipping in the same test. */
#if defined(HAVE_ALSA_PCM) && defined(HAVE_WEBRTC_AEC)
#define BOARD_ADC_VOLUME 180
#else
#define BOARD_ADC_VOLUME 200
#endif
#define BOARD_MIXER_SETUP { \
    {  2,   0, "rx sync mode Off（否则 arecord 报 pcm_read: I/O error）" }, \
    { 13, BOARD_ADC_VOLUME, "ADC Volume（AEC配方180，原管道配方200）" },          \
    { 14,  15, "MIC Gain 模拟 PGA 上限" },                                  \
    {  6,   1, "ADC HPF0 On（去直流偏置，不开 +30dB 后是明显嗡声）" },       \
    {  8,   1, "ADC HPF1 On" },                                             \
    {  4,   1, "DAC HPF On" },                                              \
    { 15,   5, "LINEOUT Gain -24dB" },                                      \
    {  0,   0, NULL } }

/* ---- 显示 ---------------------------------------------------------------
 * SPI/DBI 的 ST7789S，内核 lcd_fb 驱动注册成标准 framebuffer，RGB565。 */
#define BOARD_FB_DEV  "/dev/fb0"

/* ---- 相机 --------------------------------------------------------------- */
#define BOARD_CAM_DEV "/dev/video0"

#endif /* BOARD_CONFIG_H */
