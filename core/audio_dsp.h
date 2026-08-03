/*
 * audio_dsp.h —— 能量 VAD + speexdsp AEC/降噪
 *
 * ★ 标定必须走与判定完全相同的管道（AEC+降噪）。原始 mic 底噪实测约 4700，
 * 经 speex 降噪后只剩约 500，差 10 倍；混用尺度会让阈值完全失效（踩过）。
 * 所以标定接口收的是"已经过 dsp_process 的帧"。
 */
#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H
#include <stdint.h>

/* 帧能量（整数开方，不拖 libm） */
int  dsp_rms(const int16_t *pcm, int n);

/* AEC + 降噪。tail_ms = 滤波器覆盖的回声尾长。enable=0 则 dsp_process 直通。 */
int  dsp_init(int sample_rate, int frame_samples, int tail_ms, int enable);
void dsp_deinit(void);
/* mic 减去 ref 的回声分量 + 降噪，结果写 out。未启用时把 mic 拷给 out。 */
void dsp_process(const int16_t *mic, const int16_t *ref, int16_t *out);
int  dsp_enabled(void);

/* VAD 阈值：起说话 / 结束。 */
typedef struct { int start; int end; } vad_thresh_t;

/* 自动标定：调用方负责每帧喂"已 dsp_process 的 rms"，这里做统计。
 * 用低四分位而非均值 —— 标定期间一声杂音就能把均值拉高好几倍、阈值虚高。 */
void vad_cal_begin(void);
void vad_cal_feed(int rms);
/* 结束标定并给出阈值。start_mul/end_mul 是底噪的倍数（×10，如 25 = 2.5倍）。 */
vad_thresh_t vad_cal_end(int start_mul10, int end_mul10);

#endif
