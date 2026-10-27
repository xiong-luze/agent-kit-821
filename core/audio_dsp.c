/* audio_dsp.c —— 见 audio_dsp.h */
#include "audio_dsp.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#ifdef HAVE_WEBRTC_AEC
#include <aec_lib.h>
static void *g_vendor;
#endif

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

static int g_delay_ms = 60;
static int g_failed;

#ifdef HAVE_WEBRTC_AEC
static int vendor_configure(int rate)
{
    AecConfig config = {0};
    config.nlpMode = kAecNlpModerate;
    return WebRtcAec_Init(g_vendor, rate, rate) ||
           WebRtcAec_set_config(g_vendor, config) ? -1 : 0;
}
#endif

int dsp_init(int sample_rate, int frame_samples, int tail_ms, int enable)
{
    dsp_deinit();
    if (sample_rate <= 0 || frame_samples <= 0 || tail_ms <= 0) return -1;
    g_frame = frame_samples;
    g_failed = 0;
    g_delay_ms = 60;
    if (!enable) {
        puts("[AEC] 已禁用，播放期间靠物理关麦防回声");
        return 0;
    }
#ifdef HAVE_WEBRTC_AEC
    if (sample_rate != 16000 || frame_samples > 320 || frame_samples % 160 ||
        WebRtcAec_Create(&g_vendor) || !g_vendor || vendor_configure(sample_rate)) goto fail;
#else
    int filter_len = sample_rate * tail_ms / 1000;
    g_echo = speex_echo_state_init(frame_samples, filter_len);
    if (!g_echo || speex_echo_ctl(g_echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sample_rate)) goto fail;
#endif
    g_pre = speex_preprocess_state_init(frame_samples, sample_rate);
    if (!g_pre) goto fail;
    int on = 1, off = 0;
    if (speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_DENOISE, &on) ||
        speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_AGC, &off)) goto fail;
#ifndef HAVE_WEBRTC_AEC
    if (speex_preprocess_ctl(g_pre, SPEEX_PREPROCESS_SET_ECHO_STATE, g_echo)) goto fail;
    printf("[AEC] speex 已启用：帧 %d 样本，滤波器 %d 样本(%dms)\n",
           frame_samples, filter_len, tail_ms);
#else
    puts("[AEC] SDK WebRTC 已启用：160样本分块，moderate NLP，Speex降噪开启，AGC关闭");
#endif
    g_on = 1;
    return 0;
fail:
    fputs("[AEC] 初始化失败，未启用音频处理\n", stderr);
    dsp_deinit();
    g_failed = 1;
    return -1;
}

void dsp_deinit(void)
{
    if (g_pre) { speex_preprocess_state_destroy(g_pre); g_pre = NULL; }
    if (g_echo) { speex_echo_state_destroy(g_echo); g_echo = NULL; }
#ifdef HAVE_WEBRTC_AEC
    if (g_vendor) { WebRtcAec_Free(g_vendor); g_vendor = NULL; }
#endif
    g_on = 0;
    g_failed = 0;
}

int dsp_enabled(void) { return g_on && !g_failed; }

int dsp_set_delay(int milliseconds)
{
    if (milliseconds < 0 || milliseconds > 500) return -1;
    g_delay_ms = milliseconds;
    return 0;
}

int dsp_reset(void)
{
    if (g_failed) return -1;
    if (!g_on) return 0;
#ifdef HAVE_WEBRTC_AEC
    if (vendor_configure(16000)) { g_failed = 1; return -1; }
#else
    speex_echo_state_reset(g_echo);
#endif
    return 0;
}

int dsp_process(const int16_t *mic, const int16_t *ref, int16_t *out)
{
    if (!mic || !ref || !out || g_failed) return -1;
    if (!g_on) {
        memcpy(out, mic, (size_t)g_frame * sizeof(*out));
        return 0;
    }
#ifdef HAVE_WEBRTC_AEC
    for (int at = 0; at < g_frame; at += 160) {
        if (WebRtcAec_BufferFarend(g_vendor, ref + at, 160) ||
            WebRtcAec_Process(g_vendor, mic + at, NULL, out + at, NULL,
                              160, (int16_t)g_delay_ms, 0)) {
            memset(out, 0, (size_t)g_frame * sizeof(*out));
            g_failed = 1;
            fputs("[AEC] SDK处理失败，停止对话采播\n", stderr);
            return -1;
        }
    }
#else
    speex_echo_cancellation(g_echo, mic, ref, out);
#endif
    speex_preprocess_run(g_pre, out);
    return 0;
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
