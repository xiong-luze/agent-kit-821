/*
 * app.c —— 对话状态机（平台无关）
 *
 * 三态：聆听 → 上送 → 等待/播放 → 聆听。
 *
 * ★ 两个反复踩出来的设计约束，改之前务必看懂：
 *
 * 1) 播放期间**物理关麦**（bsp audio_mic_set(0)），不依赖 AEC。
 *    麦克风根本不在采集，AI 的声音不可能被录回去。软件 AEC 在
 *    arecord/aplay 管道这套架构上试了很多轮都没能可靠消除回声——而一问一答
 *    本来就不需要它。AEC 保留只是用来压播放期间的残留。
 *    "播完"只能靠 bsp audio_drain()（pclose 语义），不能靠写完判断：
 *    我们和播放侧之间有 64KB 管道缓冲（约 2 秒），写完远早于播完。
 *
 * 2) 一次回答可能**跨多个 TAI 事件**（MCP 拍照就是：先回"稍等我看看"，
 *    图片上传后再回识别结果）。所以判"播完"不能只看 turn_done——前一个事件
 *    的 END 到达时 ring 恰好抽空就会误判结束，把后面整段丢掉。必须再要求
 *    "距上次收到音频已安静 TAIL_QUIET_MS"。
 */
#include "app.h"
#include "audio_dsp.h"
#include "ringbuf.h"
#include "presence.h"
#include "bsp/board.h"
#include "../service/session.h"
#include "../ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/time.h>

#define MAX_TURN_MS    15000   /* 单轮说话上限，纯保险 */
#define REPLY_WAIT_MS  30000   /* 等云端回答上限 */
#define VOICE_FRAMES   3       /* 连续 3 帧(60ms)才算开始说话 */
#define TAIL_QUIET_MS  800     /* 多久没新音频才算整体播完（见文件头 ②） */
#define RING_SECONDS   40      /* 云端会突发推完十几秒 TTS，装不下就丢 */

/* 主动打招呼时交给云端的那句话。它走的是"用户输入"这条路，所以写成对模型的
 * 指令，云端照常出 NLG + TTS。刻意不提性别——本地检测只知道"有个人"，
 * 没有性别信息，硬猜错了比不猜更糟。 */
#define GREET_PROMPT "有人走到你面前了。用一句简短、自然、热情的话主动打个招呼，" \
                     "不要问对方是谁，也不要说你看到了什么。"

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int envi(const char *k, int def)
{
    const char *v = getenv(k);
    return v ? atoi(v) : def;
}

void app_cfg_defaults(app_cfg_t *c)
{
    c->sample_rate = 16000;
    c->frame_ms    = 20;
    c->vad_start   = envi("V821_VAD_START", 0);     /* 0 = 自动标定 */
    c->vad_end     = envi("V821_VAD_END",   0);
    c->vad_hang_ms = envi("V821_VAD_HANG",  1000);
    c->guard_ms    = envi("V821_GUARD_MS",  300);
    c->aec_tail_ms = envi("V821_AEC_TAIL",  300);
    c->aec_enable  = 1;
    c->greet_enable  = envi("V821_GREET", 1);
    c->greet_idle_ms = envi("V821_GREET_IDLE_MS", 60000);
    c->voice_trigger = envi("V821_VOICE_TRIGGER", 1);
}

void app_tune(const app_cfg_t *c)
{
    int fs = c->sample_rate * c->frame_ms / 1000;
    int16_t *mic = (int16_t *)calloc((size_t)fs, sizeof(int16_t));
    printf("[标定] 每 200ms 打印 RMS。先安静看底噪，再说话看峰值。Ctrl+C 退出。\n");
    int k = 0, peak = 0;
    while (bsp_get()->audio_read(mic, fs) == fs) {
        int r = dsp_rms(mic, fs);
        if (r > peak) peak = r;
        if (++k % 10 == 0) { printf("RMS=%-6d 峰值=%d\n", r, peak); peak = 0; }
    }
    free(mic);
}

/* 开机静听量底噪。必须走与判定完全相同的管道（AEC+降噪），
 * 且丢掉前 0.5 秒等降噪收敛——否则阈值会虚高好几倍。 */
static void calibrate(const app_cfg_t *c, int fs,
                      int16_t *mic, int16_t *zero, int16_t *out,
                      int *vad_start, int *vad_end)
{
    const bsp_t *b = bsp_get();
    printf("[标定] 静听 2 秒量底噪（走 AEC+降噪，与判定同一尺度），请保持安静...\n");

    for (int w = 0; w < 25; w++) {              /* 前 0.5 秒丢弃 */
        if (b->audio_read(mic, fs) != fs) break;
        b->audio_write(zero, fs);
        dsp_process(mic, zero, out);
    }
    vad_cal_begin();
    for (int n = 0; n < 75; n++) {
        if (b->audio_read(mic, fs) != fs) break;
        b->audio_write(zero, fs);
        dsp_process(mic, zero, out);
        vad_cal_feed(dsp_rms(out, fs));
    }
    vad_thresh_t t = vad_cal_end(25, 16);       /* 底噪 ×2.5 / ×1.6 */
    if (*vad_start <= 0) *vad_start = t.start;
    if (*vad_end   <= 0) *vad_end   = t.end;
}

void app_run(const app_cfg_t *c)
{
    const bsp_t *b = bsp_get();
    const session_state_t *ss = session_state();

    int fs = c->sample_rate * c->frame_ms / 1000;
    int16_t *mic  = (int16_t *)calloc((size_t)fs, sizeof(int16_t));
    int16_t *ref  = (int16_t *)calloc((size_t)fs, sizeof(int16_t));
    int16_t *out  = (int16_t *)calloc((size_t)fs, sizeof(int16_t));
    int16_t *zero = (int16_t *)calloc((size_t)fs, sizeof(int16_t));
    if (!mic || !ref || !out || !zero) return;

    int vad_start = c->vad_start, vad_end = c->vad_end;
    if (vad_start <= 0 || vad_end <= 0)
        calibrate(c, fs, mic, zero, out, &vad_start, &vad_end);
    printf("[VAD] 起说话=%d 结束=%d（静音判定 %dms）\n",
           vad_start, vad_end, c->vad_hang_ms);

    int hang_frames = c->vad_hang_ms / c->frame_ms;
    enum { ST_LISTEN, ST_SEND, ST_WAIT } st = ST_LISTEN;
    int voiced = 0, silence = 0, guard = 0;
    int64_t t_turn = 0, t_wait = 0;

    /* 主动打招呼：只在"待机够久"时才开检测，对话中一定关掉（抢 CPU） */
    int greet_on = 0;
    if (c->greet_enable) {
        presence_cfg_t pc;
        presence_cfg_defaults(&pc);
        greet_on = (presence_start(&pc) == 0);
        if (greet_on)
            printf("[聆听] 主动打招呼已开：待机 %d 秒后开始看有没有人\n",
                   c->greet_idle_ms / 1000);
    }
    int64_t t_idle = now_ms();      /* 本次进入聆听态的时刻 */

    if (!c->voice_trigger)
        printf("[聆听] ⚠ 声音触发已关（--greet-only），只走主动打招呼\n");

    printf("\n[聆听] ...\n");
    ui_set_status(c->voice_trigger ? "请说话" : "等人靠近");

    while (!ss->lost) {
        if (b->audio_read(mic, fs) != fs) break;

        /* 聆听/上送阶段不播内容，但仍要每帧喂静音给播放侧保持不断流。 */
        memset(ref, 0, (size_t)fs * sizeof(int16_t));
        b->audio_write(ref, fs);

        dsp_process(mic, ref, out);
        int rms = dsp_rms(out, fs);

        switch (st) {
        case ST_LISTEN:
            if (guard > 0) {                 /* 收尾保护期：喇叭可能还在响 */
                if (--guard == 0) printf("\n[聆听] ...\n");
                voiced = 0;
                break;
            }
            voiced = c->voice_trigger && (rms >= vad_start) ? voiced + 1 : 0;
            if (voiced >= VOICE_FRAMES) {
                if (greet_on) presence_set_enabled(0);
                session_turn_begin();
                ui_ai_begin();
                ui_set_status("识别中...");
                if (session_audio_start() != 0) {
                    fprintf(stderr, "[上送] audio_start 失败\n");
                    goto done;
                }
                session_audio_chunk(out, fs);
                silence = 0; voiced = 0; t_turn = now_ms();
                st = ST_SEND;
                break;
            }

            /* 待机够久 → 开检测；检测确认有人 → 自己开口。
             * 只在这个分支做，所以对话中（ST_SEND/ST_WAIT）检测一定是关的。 */
            if (greet_on) {
                int idle_enough = now_ms() - t_idle >= c->greet_idle_ms;
                presence_set_enabled(idle_enough);
                if (idle_enough && presence_take_trigger()) {
                    presence_set_enabled(0);
                    session_turn_begin();
                    ui_set_user("（看到有人走过来）");
                    ui_ai_begin();
                    ui_set_status("打招呼...");
                    if (session_send_text(GREET_PROMPT) != 0) {
                        fprintf(stderr, "[招呼] 上送失败\n");
                        ui_set_status("请说话");
                        break;
                    }
                    printf("[招呼] 已请云端生成招呼语\n");
                    t_wait = now_ms();
                    st = ST_WAIT;
                }
            }
            break;

        case ST_SEND:
            session_audio_chunk(out, fs);
            silence = (rms < vad_end) ? silence + 1 : 0;
            /* ★ 结束条件按可靠性排序：
             * ① 云端已开始回 TTS —— 它早就往下走了，再传只是拖延播放；
             * ② 云端 ASR eof=1 —— 云端 VAD 比本地能量门限可靠得多；
             * ③ 本地静音够久（兜底）；④ 单轮上限（保险）。
             * 只有 ③④ 的时候，本地 VAD 一没判出来就硬撑到 15 秒上限，
             * 而云端早就出了 ASR、音频也到了，白等好几秒才出声。 */
            if (ss->got_audio || ss->asr_final ||
                silence >= hang_frames || now_ms() - t_turn > MAX_TURN_MS) {
                const char *why = ss->got_audio ? "云端已开始回答"
                                : ss->asr_final ? "云端ASR判定说完"
                                : (silence >= hang_frames) ? "本地静音"
                                                           : "到单轮上限";
                session_audio_end();
                printf("\n[上送] %.1f 秒（%s）\n",
                       (double)(now_ms() - t_turn) / 1000.0, why);
                ui_set_status("思考中...");
                t_wait = now_ms();
                st = ST_WAIT;
            }
            break;

        case ST_WAIT:
            if (ss->got_audio) {
                printf("[关麦] 播放中，麦克风已关闭\n");
                ui_set_status("播放中（麦克风已关）");
                b->audio_mic_set(0);          /* 物理关麦，见文件头 ① */

                while (!ss->lost) {
                    if (ringbuf_pop_frame(ref, (size_t)fs) > 0) {
                        b->audio_write(ref, fs);
                    } else if (ss->turn_done &&
                               now_ms() - ss->last_audio_ms > TAIL_QUIET_MS) {
                        break;                /* 真的没有后续了，见文件头 ② */
                    } else if (!ss->turn_done &&
                               now_ms() - ss->last_audio_ms > REPLY_WAIT_MS) {
                        printf("\n[播放] 等后续音频超时\n");
                        break;
                    } else {
                        usleep(5000);         /* 等网络补数据 */
                    }
                }
                b->audio_drain();             /* 阻塞到真正播完 */
                usleep((useconds_t)c->guard_ms * 1000);   /* 等房间混响散尽 */
                b->audio_mic_set(1);

                printf("[开麦] 播放结束\n\n[聆听] ...\n");
                ui_set_status("请说话");
                voiced = 0; silence = 0;
                /* 真聊过一轮才重新计待机、并要求人先离开再回来 */
                t_idle = now_ms();
                if (greet_on) presence_rearm();
                st = ST_LISTEN;
                break;
            }
            /* ★ 下面两条出口是"这轮什么都没产出"，最常见的成因是环境噪声触发了
             * 上送、云端 ASR 返回空文本。**故意不重置 t_idle**：否则噪声每隔几秒
             * 来一次假对话，待机计时永远数不到 greet_idle_ms，主动打招呼那条路
             * 就被彻底堵死。真说过话的那轮走上面 got_audio 分支，才算重新待机。 */
            if (ss->turn_done && ringbuf_avail() == 0) {
                guard = c->guard_ms / c->frame_ms;
                voiced = 0;
                st = ST_LISTEN;
            } else if (!ss->turn_done && now_ms() - t_wait > REPLY_WAIT_MS) {
                printf("\n[等待] 超时\n[聆听] ...\n");
                st = ST_LISTEN;
            }
            break;
        }
    }

done:
    fprintf(stderr, "[退出] 连接断开\n");
    if (greet_on) presence_stop();
    free(mic); free(ref); free(out); free(zero);
}
