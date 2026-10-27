/*
 * session.h —— 涂鸦 AI 会话（连云引导 + TAI 收发）
 *
 * 对应团队 ESP 仓库设计文档里规划但尚未实现的 ak_ai_session 服务组件。
 *
 * 职责边界：
 *   - 这里负责：取 session token、解析连接参数、建 TAI 上下文、收 TAI 回调、
 *     把 TTS 音频塞进 ringbuf、把文本交给上层显示。
 *   - 这里不负责：什么时候开始/结束一轮（那是 core/app.c 的状态机）。
 *
 * 回调跑在 TAI 接收线程上，主循环使用 session_snapshot() 取得同步的状态副本。
 * 全双工模式通过有界工作队列发送，音频线程不执行阻塞网络调用。
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include <stddef.h>
#include "tuya_ai.h"

/* 由 TAI 接收线程更新，主循环只读 */
typedef struct {
    volatile int lost;            /* 连接断了 */
    volatile int turn_done;       /* 收到 TAI_EVT_END */
    volatile int got_audio;       /* 本轮收到过 TTS 音频 */
    volatile int asr_final;       /* 云端 ASR eof=1：它判定这句说完了 */
    volatile int64_t last_audio_ms;  /* 最后一次收到音频的时刻 */
    unsigned break_seq;            /* 收到有效服务端 ChatBreak 时递增 */
} session_state_t;

/* UI callbacks may be NULL. They run under the session state mutex to serialize
 * generation checks and turn clears. Keep them short and non-reentrant: only
 * copy UI state; do not call session APIs, perform network I/O, or wait. */
typedef struct {
    void (*on_user_text)(const char *s);   /* ASR 识别到的用户话 */
    void (*on_ai_text)(const char *s);     /* NLG 回答，分句追加 */
    void (*on_emotion)(const char *s);     /* SKILL/emo 的 emotion[0] */
    void (*on_status)(const char *s);
    void (*on_turn_begin)(void);          /* serialized clear for duplex post_start */
} session_ui_t;

/* 连云：iot_client 登录 → 取 session token → 解析 → tai_connect。0 = 成功。 */
int  session_connect(const char *devid, const char *secret_key,
                     const char *local_key, const session_ui_t *ui);
void session_close(void);

const session_state_t *session_state(void);
/* session_state() is the legacy, unsynchronized pointer. New callers must use
 * this snapshot, including on riscv32 where int64 reads need synchronization. */
void session_snapshot(session_state_t *out);
/* Pop PCM and snapshot its server-break epoch under the same state->ring lock.
 * Query with samples=0/out=NULL to get only the epoch without consuming audio.
 * On an epoch change, flush the DAC before submitting the returned PCM; do NOT
 * clear the ring again, because it may already contain the next valid reply.
 * Returns actual PCM samples; valid reads are zero-padded like ringbuf_pop_frame.
 * Missing epoch output or NULL PCM with samples>0 is a no-op returning zero. */
size_t session_pop_audio(int16_t *out, size_t samples, unsigned *break_seq);
tai_ctx_t             *session_ctx(void);   /* MCP 要拿它回包 */

/* Opt-in duplex sender. Enable after connect; disable before tearing down the
 * SDK context. Only one application/audio thread owns these posting APIs.
 * Posts copy PCM, never execute the transport on the caller thread. Up to
 * 320 samples per chunk, 160 pending commands (2s jitter + up to 1s pre-roll);
 * overflow/transport failure is
 * explicit and persistent until disable/re-enable. Keep preroll <=1 second.
 * post_start resets turn flags itself; do not also call session_turn_begin().
 * Stop/flush local playback BEFORE post_interrupt(). Then post_start() and
 * new PCM can follow immediately, without waiting for a cloud acknowledgement.
 * Receive admission requires fresh ASR event IDs; see turn_guard.h limits. */
int session_duplex_enable(void);
void session_duplex_disable(void);
int session_post_start(void);
int session_post_chunk(const int16_t *pcm, int frames);
int session_post_end(void);
int session_post_interrupt(void);
int session_tx_error(void);

#define SESSION_PHOTO_CANCELED 1
/* Photo worker only: waits for audio-event ownership, rechecks the generation,
 * then sends one complete image/text event. Returns 0, +1 stale/canceled, or a
 * negative error. Never call from the realtime audio thread. Generation 0 is
 * the legacy nonduplex path. Captured generations must not be relabeled. */
int session_send_photo_for_generation(uint64_t generation, const char *hint,
                                      const uint8_t *jpeg, size_t bytes,
                                      uint16_t width, uint16_t height);
int session_generation_is_current(uint64_t generation);
void session_status_for_generation(uint64_t generation, const char *status);

/* Legacy synchronous path: clear -> start -> chunks -> end. These direct SDK
 * APIs return -EPERM while duplex mode is enabled; use the queue above instead. */
void session_turn_begin(void);
int  session_audio_start(void);
int  session_audio_chunk(const int16_t *pcm, int frames);
int  session_audio_end(void);

/* 设备主动发起的一轮：直接把一句文本当作这轮的输入交给云端，云端照常出
 * NLG + TTS。用于"看到有人主动打招呼"这类不是用户说话触发的场合。
 * 调用前同样要 session_turn_begin()。 */
int  session_send_text(const char *utf8);

#endif /* SESSION_H */
