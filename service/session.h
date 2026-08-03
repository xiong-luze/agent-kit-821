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
 * 回调跑在 TAI 的接收线程上，所以对外只暴露一组 volatile 标志（session_state_t），
 * 主循环轮询它们，不做跨线程的复杂交互。
 */
#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>
#include "tuya_ai.h"

/* 由 TAI 接收线程更新，主循环只读 */
typedef struct {
    volatile int lost;            /* 连接断了 */
    volatile int turn_done;       /* 收到 TAI_EVT_END */
    volatile int got_audio;       /* 本轮收到过 TTS 音频 */
    volatile int asr_final;       /* 云端 ASR eof=1：它判定这句说完了 */
    volatile int64_t last_audio_ms;  /* 最后一次收到音频的时刻 */
} session_state_t;

/* 上层提供的显示回调（可为 NULL）：把文本/情绪送到界面 */
typedef struct {
    void (*on_user_text)(const char *s);   /* ASR 识别到的用户话 */
    void (*on_ai_text)(const char *s);     /* NLG 回答，分句追加 */
    void (*on_emotion)(const char *s);     /* SKILL/emo 的 emotion[0] */
    void (*on_status)(const char *s);
} session_ui_t;

/* 连云：iot_client 登录 → 取 session token → 解析 → tai_connect。0 = 成功。 */
int  session_connect(const char *devid, const char *secret_key,
                     const char *local_key, const session_ui_t *ui);
void session_close(void);

const session_state_t *session_state(void);
tai_ctx_t             *session_ctx(void);   /* MCP 要拿它回包 */

/* 一轮的上送。清标志 → start → 若干 chunk → end。 */
void session_turn_begin(void);
int  session_audio_start(void);
int  session_audio_chunk(const int16_t *pcm, int frames);
int  session_audio_end(void);

/* 设备主动发起的一轮：直接把一句文本当作这轮的输入交给云端，云端照常出
 * NLG + TTS。用于"看到有人主动打招呼"这类不是用户说话触发的场合。
 * 调用前同样要 session_turn_begin()。 */
int  session_send_text(const char *utf8);

#endif /* SESSION_H */
