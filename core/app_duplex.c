/* Continuous audio loop. Cloud sends and keyword inference run on their own workers. */
#include "app.h"
#include "audio_dsp.h"
#include "dialog.h"
#include "echo_guard.h"
#include "pcm_history.h"
#include "ringbuf.h"
#include "wake_word.h"
#include "bsp/board.h"
#include "../service/session.h"
#include "../ui/ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static int64_t monotonic_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

typedef struct { int enabled; int64_t wall, cpu; } audio_trace_t;
static void audio_trace_point(audio_trace_t *trace, const char *phase)
{
    if (!trace->enabled) return;
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    int64_t cpu = (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    int64_t wall = monotonic_ms();
    if (trace->wall && wall - trace->wall >= 35)
        fprintf(stderr, "[时序诊断] phase=%s wall_ms=%lld cpu_ms=%lld\n", phase,
                (long long)(wall - trace->wall), (long long)(cpu - trace->cpu));
    trace->wall = wall;
    trace->cpu = cpu;
}

static int uplink_result(const char *operation, int result)
{
    if (result)
        fprintf(stderr, "[上送诊断] operation=%s rc=%d\n", operation, result);
    return result;
}

static int device_failure(const char *operation, int error)
{
    int rebuild = error == BSP_ERR_AUDIO_DISCONTINUITY;
    fprintf(stderr, "[采播诊断] operation=%s rc=%d rebuild=%d\n", operation, error, rebuild);
    return rebuild ? APP_DUPLEX_REBUILD_AUDIO : APP_DUPLEX_ERROR;
}

static int calibrate_duplex(const bsp_t *b, int16_t *mic, int16_t *ref, int16_t *out,
                            int fs, int *start, int *end)
{
    puts("[全双工] 静听2秒标定底噪，请保持安静");
    vad_cal_begin();
    for (int i = 0; i < 100; i++) {
        int captured = b->audio_read(mic, fs);
        if (captured != fs) return captured < 0 ? captured : -EIO;
        int written = b->audio_write(ref, fs);
        if (written != fs) return written < 0 ? written : -EIO;
        if (dsp_process(mic, ref, out)) return -1;
        if (i >= 25) vad_cal_feed(dsp_rms(out, fs));
    }
    vad_thresh_t threshold = vad_cal_end(25, 16);
    if (*start <= 0) *start = threshold.start;
    if (*end <= 0) *end = threshold.end;
    return 0;
}

int app_run_duplex(const app_cfg_t *c)
{
    const bsp_t *b = bsp_get();
    if (!(b->caps & BSP_CAP_AUDIO_DUPLEX) || !b->audio_flush || !b->audio_pending ||
        !dsp_enabled() || app_cfg_validate(c)) {
        fprintf(stderr, "[全双工] 需要进程内ALSA后端、有效配置和已初始化的AEC\n");
        return -1;
    }
    int rc = -1, wake_started = 0, audio_started = 0;
    int fs = c->sample_rate * c->frame_ms / 1000;
    size_t history_size = (size_t)c->sample_rate * c->pre_roll_ms / 1000;
    pcm_history_t history = {0};
    int16_t *mic = calloc((size_t)fs, sizeof(*mic));
    int16_t *ref = calloc((size_t)fs, sizeof(*ref));
    int16_t *out = calloc((size_t)fs, sizeof(*out));
    int16_t *pre = calloc(history_size, sizeof(*pre));
    if (!mic || !ref || !out || !pre || pcm_history_init(&history, history_size)) goto done;

    if (c->wake_enabled) {
        wake_word_config_t cfg;
        wake_word_config_defaults(&cfg);
        cfg.model_dir = c->wake_model;
        cfg.dictionary = c->wake_dictionary;
        cfg.phrase = c->wake_word;
        cfg.threshold = c->wake_threshold;
        cfg.activity_min_rms = c->wake_min_rms;
        if (wake_word_start(&cfg)) {
            fprintf(stderr, "[唤醒] 本地识别器初始化失败，未开始采集或上传\n");
            goto done;
        }
        wake_started = 1;
    }
    if (session_duplex_enable()) goto done;
    /* No device I/O is started before model load, network connect, or MCP setup. */
    int device_rc = b->audio_start(c->sample_rate);
    if (device_rc != BSP_OK) { rc = device_failure("start", device_rc); goto done; }
    audio_started = 1;
    int start = c->vad_start, end = c->vad_end;
    if (start <= 0 || end <= 0) {
        device_rc = calibrate_duplex(b, mic, ref, out, fs, &start, &end);
        if (device_rc) { rc = device_failure("calibration", device_rc); goto done; }
    }
    dialog_config_t config = {
        .frame_ms = c->frame_ms, .wake_enabled = c->wake_enabled, .wake_hold_ms = c->wake_hold_ms,
        .vad_start = start, .vad_end = end, .vad_hang_ms = c->vad_hang_ms,
        .barge_ms = c->barge_ms, .max_turn_ms = 15000, .reply_timeout_ms = 30000,
        .tail_quiet_ms = 800
    };
    dialog_t dialog;
    if (dialog_init(&dialog, &config, monotonic_ms())) goto done;
    echo_guard_t echo_guard;
    echo_guard_init(&echo_guard);
    ui_set_status(c->wake_enabled ? c->wake_word : "请说话");
    printf("[全双工] AEC持续工作；唤醒=%s；本地打断确认=%dms；预录=%dms\n",
           c->wake_enabled ? c->wake_word : "关闭", c->barge_ms, c->pre_roll_ms);
    if (c->greet_enable) puts("[全双工] 人形主动招呼暂停；MCP拍照仍可用");
    unsigned break_seq = 0;
    int64_t speech_tail_until = 0, last_reply_activity = monotonic_ms();
    const char *trace_env = getenv("V821_VOICE_TRACE");
    audio_trace_t trace = {.enabled = trace_env && !strcmp(trace_env, "1")};
    audio_trace_point(&trace, "begin");
    unsigned wake_trace_frames = 0;
    int wake_trace_mic_peak = 0, wake_trace_dsp_peak = 0;
    int wake_voiced_samples = 0, wake_activity_left = 0;

    for (;;) {
        session_state_t state;
        session_snapshot(&state);
        audio_trace_point(&trace, "snapshot");
        if (state.lost || session_tx_error()) break;
        audio_trace_point(&trace, "tx-status");
        int captured = b->audio_read(mic, fs);
        audio_trace_point(&trace, "capture");
        if (captured != fs) { rc = device_failure("capture", captured); goto done; }
        int waiting = dialog.state == DIALOG_WAIT || dialog.state == DIALOG_PLAY;
        unsigned epoch;
        size_t played = session_pop_audio(waiting ? ref : NULL, waiting ? (size_t)fs : 0, &epoch);
        audio_trace_point(&trace, "pop-audio");
        if (epoch != break_seq) {
            break_seq = epoch;
            /* The callback already cleared old queued PCM. This atomically popped
             * frame belongs to epoch and must survive the delayed DAC flush. */
            device_rc = b->audio_flush();
            if (device_rc) { rc = device_failure("flush", device_rc); goto done; }
            if (dsp_reset()) goto done;
            speech_tail_until = 0;
        }
        if (!waiting) memset(ref, 0, (size_t)fs * sizeof(*ref));
        device_rc = b->audio_write(ref, fs);
        if (device_rc != fs) { rc = device_failure("playback", device_rc); goto done; }
        audio_trace_point(&trace, "playback");
        int pending = b->audio_pending();
        if (pending < 0) { rc = device_failure("pending", pending); goto done; }
        int64_t now = monotonic_ms();
        if (played)
            speech_tail_until = now + ((int64_t)pending * 1000 / c->sample_rate) + c->guard_ms;
        if (dsp_set_delay(c->frame_ms + pending * 1000 / c->sample_rate) ||
            dsp_process(mic, ref, out)) goto done;
        audio_trace_point(&trace, "dsp");
        echo_guard_result_t echo_evidence;
        if (echo_guard_process(&echo_guard, ref, out, (size_t)fs, &echo_evidence)) goto done;
        audio_trace_point(&trace, "echo-guard");
        pcm_history_push(&history, out, (size_t)fs);

        int wake = 0;
        if (wake_started && dialog.state == DIALOG_SLEEP) {
            int mic_rms = dsp_rms(mic, fs);
            if (wake_activity_left > fs) wake_activity_left -= fs;
            else wake_activity_left = 0;
            /* A weak-noise KWS match is not speech evidence. Require60ms of
             * raw activity, and retain it for1s so delayed keyword output and
             * final unvoiced consonants are not truncated. The sample clock
             * is independent of thread scheduling and supports paced replays. */
            if (mic_rms >= c->wake_min_rms) {
                int confirm = c->sample_rate * 60 / 1000;
                if (wake_voiced_samples < confirm) wake_voiced_samples += fs;
                if (wake_voiced_samples >= confirm) wake_activity_left = c->sample_rate;
            } else wake_voiced_samples = 0;
            if (trace.enabled) {
                int out_rms = dsp_rms(out, fs);
                if (mic_rms > wake_trace_mic_peak) wake_trace_mic_peak = mic_rms;
                if (out_rms > wake_trace_dsp_peak) wake_trace_dsp_peak = out_rms;
                if (++wake_trace_frames == 100) {
                    fprintf(stderr, "[唤醒诊断] frames=100 max_mic_rms=%d max_dsp_rms=%d\n",
                            wake_trace_mic_peak, wake_trace_dsp_peak);
                    wake_trace_frames = 0;
                    wake_trace_mic_peak = wake_trace_dsp_peak = 0;
                }
            }
            /* SLEEP never submits speaker content. Preserve the keyword model's
             * input features here: AEC/NLP altered a known positive into a miss.
             * Pre-roll, cloud uplink and barge detection still use processed out. */
            if (wake_word_feed(mic, (size_t)fs)) goto done;
            wake = wake_word_poll();
            if (wake < 0) goto done;
            if (wake && c->wake_min_rms && !wake_activity_left) {
                if (trace.enabled)
                    fprintf(stderr, "[唤醒诊断] rejected_low_activity=1 mic_rms=%d floor=%d\n",
                            mic_rms, c->wake_min_rms);
                wake = 0;
            }
        }
        session_snapshot(&state);
        audio_trace_point(&trace, "wake-and-snapshot");
        /* session timestamps use its monotonic clock; ignore pre-turn activity. */
        if (state.last_audio_ms > last_reply_activity) last_reply_activity = state.last_audio_ms;
        dialog_input_t in = {
            .now_ms = now, .reply_quiet_ms = now - last_reply_activity,
            .rms = dsp_rms(out, fs), .wake = wake, .asr_final = state.asr_final,
            .reply_audio = state.got_audio, .reply_done = state.turn_done,
            .echo_likely = echo_evidence.reference_active && !echo_evidence.near_allowed,
            .playback_active = ringbuf_avail() > 0 || now < speech_tail_until
        };
        unsigned actions = dialog_step(&dialog, &in);
        if (trace.enabled && (actions & ~DIALOG_AUDIO))
            fprintf(stderr, "[对话诊断] t=%lld state=%d actions=%u rms=%d asr=%d audio=%d done=%d\n",
                    (long long)now, (int)dialog.state, actions, in.rms,
                    in.asr_final, in.reply_audio, in.reply_done);
        if (actions & DIALOG_INTERRUPT) {
            /* Stop the speaker FIRST. No network call happens before this flush. */
            device_rc = b->audio_flush();
            if (device_rc) { rc = device_failure("flush", device_rc); goto done; }
            ringbuf_clear();
            if (dsp_reset()) goto done;
            speech_tail_until = 0;
            printf("[打断] 本地已清播放缓冲，t=%lldms\n", (long long)now);
            if (uplink_result("interrupt", session_post_interrupt())) goto done;
        }
        if (actions & DIALOG_START) {
            wake_voiced_samples = wake_activity_left = 0;
            if (wake_started && wake_word_set_enabled(0)) goto done;
            ui_set_status("识别中...");
            if (uplink_result("start", session_post_start())) goto done;
            audio_trace_point(&trace, "post-start");
            last_reply_activity = now;
            size_t n = pcm_history_read(&history, pre, history_size);
            for (size_t i = 0; i < n; i += (size_t)fs) {
                size_t count = n - i < (size_t)fs ? n - i : (size_t)fs;
                if (uplink_result("pre-roll", session_post_chunk(pre + i, (int)count))) goto done;
            }
            pcm_history_clear(&history);
        } else if ((actions & DIALOG_AUDIO) && uplink_result("chunk", session_post_chunk(out, fs))) goto done;
        if ((actions & DIALOG_END) && uplink_result("end", session_post_end())) goto done;
        if (actions & DIALOG_FINISHED) ui_set_status("请说话");
        if (actions & DIALOG_SLEPT) {
            wake_voiced_samples = wake_activity_left = 0;
            pcm_history_clear(&history);
            if (wake_started && wake_word_set_enabled(1)) goto done;
            ui_set_status(c->wake_word);
        }
        audio_trace_point(&trace, "post-controls");
    }
    fprintf(stderr, "[全双工] 连接或发送线程结束\n");
done:
    /* Stop local I/O before joining potentially slow transport/inference workers. */
    if (audio_started) b->audio_stop();
    session_state_t final_state;
    session_snapshot(&final_state);
    int tx_error = session_tx_error();
    if (rc != APP_DUPLEX_REBUILD_AUDIO && audio_started && (final_state.lost || tx_error))
        rc = APP_DUPLEX_RECONNECT;
    if (rc == APP_DUPLEX_REBUILD_AUDIO) ui_set_status("音频中断，恢复中...");
    else if (rc == APP_DUPLEX_RECONNECT) ui_set_status("网络连接中断，重连中...");
    else ui_set_status("语音服务异常");
    fprintf(stderr, "[退出诊断] rc=%d lost=%d tx_error=%d audio_started=%d\n",
            rc, final_state.lost, tx_error, audio_started);
    session_duplex_disable();
    if (wake_started) wake_word_stop();
    pcm_history_free(&history);
    free(mic); free(ref); free(out); free(pre);
    return rc;
}
