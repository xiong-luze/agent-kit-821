/*
 * ringbuf.h —— TTS 播放环形缓冲（接收线程写，主循环读）
 *
 * 要够大：云端会在 6 秒内把 12 秒以上的 TTS 突发推完，装不下就被丢弃，
 * 回答会播到一半没了（实测踩过，原先 4 秒不够）。
 */
#ifndef RINGBUF_H
#define RINGBUF_H
#include <stdint.h>
#include <stddef.h>

void   ringbuf_init(int sample_rate, int seconds);
size_t ringbuf_avail(void);
void   ringbuf_push(const int16_t *pcm, size_t n);
/* 取一帧；不足补静音。返回实际取到的样本数（0 = 空） */
size_t ringbuf_pop_frame(int16_t *out, size_t frame_samples);
void   ringbuf_clear(void);
#endif
