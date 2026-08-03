/* audio_dsp.c —— 见 audio_dsp.h */
#include "audio_dsp.h"
#include <stdio.h>
#include <string.h>

#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"

static SpeexEchoState       *g_echo = NULL;
static SpeexPreprocessState *g_pre  = NULL;
static int g_frame = 0;
static int g_on    = 0;

int dsp_rms(const int16_t *pcm, int n)
{
    int64_t acc = 0;
    for (int i = 0; i < n; i++) acc += (int64_t)pcm[i] * pcm[i];
    int64_t mean = acc / (n > 0 ? n : 1), r = 0, bit = 1LL << 30;
    while (bit > mean) bit >>= 2;
    while (bit) {
        if (mean >= r + bit) { mean -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (int)r;
}

int dsp_init(int sample_rate, int frame_samples, int tail_ms, int enable)
{
    g_frame = frame_samples;
    if (!enable) {
        printf("[AEC] 已禁用，播放期间靠物理关麦防回声\n");
        g_on = 0;
        return 0;
    }
    int filter_len = sample_rate * tail_ms / 1000;
    g_echo = speex_echo_state_init(frame_samples, filter_len);
    if (!g_echo) {
        fprintf(stderr, "[AEC] 初始化失败，退回物理关麦\n");
        g_on = 0;
        return -1;
    }
    int rate = sample_rate;
    speex_echo_ctl(g_echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    g_pre = speex_preprocess_state_init(frame_samples, sample_rate);
    if (g_pre) {
        int on = 1, off = 0;
        speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_DENOISE, &on);
        /* AGC 必须关：mixer 里已经加了 +30dB 数字增益，再叠自动增益会把底噪
         * 和 AEC 残留一起顶上来（全志 AEC 文档 §4.6 也警告增益过大伤 AEC）。 */
        speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_AGC, &off);
        speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_ECHO_STATE, g_echo);
    }
    printf("[AEC] speex 已启用：帧 %d 样本，滤波器 %d 样本(%dms)\n",
           frame_samples, filter_len, tail_ms);
    g_on = 1;
    return 0;
}

void dsp_deinit(void)
{
    if (g_pre)  { speex_preprocess_state_destroy(g_pre);  g_pre  = NULL; }
    if (g_echo) { speex_echo_state_destroy(g_echo); g_echo = NULL; }
    g_on = 0;
}

int dsp_enabled(void) { return g_on; }

void dsp_process(const int16_t *mic, const int16_t *ref, int16_t *out)
{
    if (!g_on || !g_echo) {
        memcpy(out, mic, (size_t)g_frame * sizeof(int16_t));
        return;
    }
    speex_echo_cancellation(g_echo, mic, ref, out);
    if (g_pre) speex_preprocess_run(g_pre, out);
}

/* -- VAD 标定 ------------------------------------------------------------ */
#define CAL_MAX 128
static int g_samp[CAL_MAX], g_n;

void vad_cal_begin(void) { g_n = 0; }

void vad_cal_feed(int rms) { if (g_n < CAL_MAX) g_samp[g_n++] = rms; }

vad_thresh_t vad_cal_end(int start_mul10, int end_mul10)
{
    vad_thresh_t t = { 0, 0 };
    if (g_n <= 0) { t.start = 1250; t.end = 800; return t; }

    int lo = g_samp[0], hi = g_samp[0]; long acc = 0;
    for (int i = 0; i < g_n; i++) {
        acc += g_samp[i];
        if (g_samp[i] < lo) lo = g_samp[i];
        if (g_samp[i] > hi) hi = g_samp[i];
    }
    for (int a = 0; a < g_n; a++)          /* 排序取低四分位 */
        for (int b = a + 1; b < g_n; b++)
            if (g_samp[b] < g_samp[a]) { int x = g_samp[a]; g_samp[a] = g_samp[b]; g_samp[b] = x; }
    int floor_rms = g_samp[g_n / 4];

    /* ★ 起说话阈值必须高于标定期间观测到的**峰值**，不能只高于低四分位。
     * 踩过的坑：floor=155 → start=155*2.5=387，而底噪尖峰是 511 —— 结果环境
     * 噪声自己不停触发上送，每轮 ASR 都返回空文本，既不出声也把"待机够久才
     * 主动打招呼"那条路彻底堵死（每次假对话都在推迟待机计时）。
     * 低四分位是为了不让偶发杂音抬高底噪估计，但抬完之后仍要盖住实测峰值。 */
    int base = floor_rms;
    int need = hi * 12 / 10;                        /* 峰值留 20% 余量 */
    if (base * start_mul10 / 10 < need) {
        int bumped = need * 10 / start_mul10;
        /* 但也不能被一声门响拽到听不见人说话。这里的信噪比只有约 12dB，
         * 封顶 8 倍底噪；超了说明标定环境不可用，只能提示手动指定。 */
        int cap = floor_rms * 8 * 10 / start_mul10;
        if (bumped > cap) bumped = cap;
        base = bumped;
    }

    t.start = base * start_mul10 / 10;
    t.end   = base * end_mul10   / 10;
    printf("[标定] 降噪后底噪 RMS=%d（低四分位；区间 %d~%d，均值 %ld）\n",
           floor_rms, lo, hi, acc / g_n);
    printf("       → 起说话=%d 结束=%d\n", t.start, t.end);
    if (base != floor_rms)
        printf("       ↑ 已抬高：原本 %d 盖不住底噪尖峰 %d（噪声会自己触发上送）\n",
               floor_rms * start_mul10 / 10, hi);
    if (t.start <= hi)
        printf("       ⚠ 阈值 %d 仍低于底噪尖峰 %d —— 环境太吵，"
               "换安静环境重跑或用 V821_VAD_START 手动指定\n", t.start, hi);
    return t;
}
