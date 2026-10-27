/* Run the actual audio loop against deterministic device/network/KWS boundaries.
 * Acoustic AEC and real model tests live separately; these stubs do not prove them. */
#include "core/app.h"
#include "core/audio_dsp.h"
#include "core/wake_word.h"
#include "core/ringbuf.h"
#include "bsp/board.h"
#include "service/session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

static int frame, started, stopped, wake_started, wake_enabled = 1;
static int starts, chunks, interrupts, flushes, last_operation, first_tx_frame;
static int first_sample = -1, last_pre_sample, mic_mutes, disabled, wake_stopped;
static int inject_server_break, fresh_after_break, dsp_offset, quiet_hit;
static int device_error, device_operation, expected_device_result;
static session_state_t state;

static int64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int audio_start(int rate)
{
    assert(rate == 16000 && wake_started);
    started++;
    return 0;
}
static int audio_read(int16_t *pcm, int n)
{
    assert(started);
    frame++;
    if (device_operation == 1 && frame == 30) return device_error;
    if (inject_server_break && frame == 42) {
        /* Break + fresh TTS both arrive while the audio thread was reading. */
        ringbuf_clear();
        state.break_seq++;
        int16_t fresh[320];
        for (int i = 0; i < 320; i++) fresh[i] = 999;
        assert(ringbuf_push(fresh, 320) == 320);
    }
    int sample = ((frame >= 15 && frame <= 28) || (frame >= 50 && frame <= 65)) ? 2000 : frame;
    if (quiet_hit == 1) sample = 50;
    if (quiet_hit == 2 || quiet_hit == 4) sample = frame >= 15 && frame <= 18 ? 2000 : 50;
    if (quiet_hit == 3) sample = frame == 19 ? 2000 : 50;
    for (int i = 0; i < n; i++)
        pcm[i] = (int16_t)(sample == 2000 && i && i < n - 1 ?
                          ((i * 31 + frame * 7) % 17 - 8) * 300 : sample);
    return n;
}
static int audio_write(const int16_t *pcm, int n)
{
    if (device_operation == 2 && frame == 30) return device_error;
    if (inject_server_break && frame == 42) {
        assert(last_operation == 1); /* Flush happens before this new epoch. */
        assert(pcm[0] == 999);       /* New TTS survives the delayed DAC flush. */
        fresh_after_break++;
    }
    return n;
}
static int audio_pending(void) { return device_operation == 3 && frame == 30 ? device_error : 640; }
static int audio_flush(void)
{ flushes++; last_operation = 1; return device_operation == 4 ? device_error : 0; }
static void audio_stop(void) { stopped++; }
static void audio_mic_set(int on) { (void)on; mic_mutes++; }
static const bsp_t board = {
    .caps = BSP_CAP_AUDIO | BSP_CAP_AUDIO_DUPLEX,
    .audio_start = audio_start, .audio_read = audio_read, .audio_write = audio_write,
    .audio_stop = audio_stop, .audio_mic_set = audio_mic_set,
    .audio_flush = audio_flush, .audio_pending = audio_pending
};
const bsp_t *bsp_get(void) { return &board; }
int dsp_enabled(void) { return 1; }
int dsp_reset(void) { return 0; }
int dsp_set_delay(int ms) { assert(ms >= 0 && ms <= 500); return 0; }
int dsp_process(const int16_t *mic, const int16_t *ref, int16_t *out)
{
    (void)ref;
    for (int i = 0; i < 320; i++) out[i] = (int16_t)(mic[i] + dsp_offset);
    return 0;
}
int dsp_rms(const int16_t *pcm, int n) { (void)n; return pcm[0]; }
void vad_cal_begin(void) {}
void vad_cal_feed(int rms) { (void)rms; }
vad_thresh_t vad_cal_end(int a, int b) { (void)a; (void)b; return (vad_thresh_t){1000, 600}; }
void ui_set_status(const char *s) { assert(s); }
void ui_ai_begin(void) {}

void wake_word_config_defaults(wake_word_config_t *cfg) { memset(cfg, 0, sizeof(*cfg)); }
int wake_word_start(const wake_word_config_t *cfg)
{
    assert(!started && !strcmp(cfg->phrase, "你好涂鸦"));
    wake_started++;
    return 0;
}
int wake_word_feed(const int16_t *pcm, size_t n)
{
    assert(pcm && n == 320 && wake_enabled);
    int raw_sample = quiet_hit ? 50 : frame >= 15 && frame <= 28 ? 2000 : frame;
    if (quiet_hit == 2 || quiet_hit == 4) raw_sample = frame >= 15 && frame <= 18 ? 2000 : 50;
    if (quiet_hit == 3) raw_sample = frame == 19 ? 2000 : 50;
    assert(pcm[0] == raw_sample); /* KWS gets original features; cloud pre-roll stays processed. */
    return 0;
}
int wake_word_poll(void) { return frame == (quiet_hit == 2 ? 80 : quiet_hit == 4 ? 40 : 20); }
int wake_word_set_enabled(int enabled) { wake_enabled = enabled; return 0; }
void wake_word_stop(void) { assert(stopped); wake_stopped++; }

void session_snapshot(session_state_t *snapshot)
{
    *snapshot = state;
    if (frame >= 90) snapshot->lost = 1;
}
size_t session_pop_audio(int16_t *pcm, size_t samples, unsigned *epoch)
{
    *epoch = state.break_seq;
    return samples ? ringbuf_pop_frame(pcm, samples) : 0;
}
int session_duplex_enable(void) { assert(!started); return 0; }
void session_duplex_disable(void) { assert(stopped); disabled++; }
int session_tx_error(void) { return 0; }
int session_post_start(void)
{
    assert(frame >= 20); /* Nothing is uploaded before the keyword hit. */
    if (!starts) first_tx_frame = frame;
    starts++;
    unsigned epoch = state.break_seq;
    memset(&state, 0, sizeof(state));
    state.break_seq = epoch;
    return 0;
}
int session_post_chunk(const int16_t *pcm, int n)
{
    assert(n > 0 && n <= 320 && starts);
    if (chunks == 0) first_sample = pcm[0];
    if (frame == 20) last_pre_sample = pcm[n - 1];
    chunks++;
    return 0;
}
int session_post_end(void)
{
    state.got_audio = state.turn_done = 1;
    state.last_audio_ms = now_ms();
    int16_t reply[320];
    for (int i = 0; i < 320; i++) reply[i] = 123;
    for (int i = 0; i < 20; i++) assert(ringbuf_push(reply, 320) == 320);
    return 0;
}
int session_post_interrupt(void)
{
    assert(last_operation == 1); /* DAC stopped before any cloud request. */
    assert(ringbuf_avail() == 0);
    interrupts++;
    last_operation = 2;
    return 0;
}

static void run_case(int server_break, int offset, int quiet)
{
    frame = started = stopped = wake_started = starts = chunks = interrupts = flushes = 0;
    last_operation = first_tx_frame = last_pre_sample = mic_mutes = disabled = wake_stopped = 0;
    first_sample = -1;
    wake_enabled = 1;
    inject_server_break = server_break;
    dsp_offset = offset;
    quiet_hit = quiet;
    fresh_after_break = 0;
    memset(&state, 0, sizeof(state));
    app_cfg_t cfg;
    app_cfg_defaults(&cfg);
    cfg.full_duplex = 1;
    cfg.wake_enabled = 1;
    cfg.wake_model = "test-model-boundary";
    cfg.wake_dictionary = "test-dictionary-boundary";
    cfg.vad_start = 1000;
    cfg.vad_end = 600;
    cfg.vad_hang_ms = 100;
    assert(ringbuf_init(16000, 2) == 0);
    int result = app_run_duplex(&cfg);
    assert(started == 1 && stopped == 1 && disabled == 1 && wake_stopped == 1);
    if (device_error) {
        assert(result == expected_device_result);
        ringbuf_free();
        return;
    }
    assert(result == APP_DUPLEX_RECONNECT); /* Fake disconnect is retryable. */
    if (quiet) {
        if (quiet == 4) assert(starts == 1 && first_tx_frame == 40 && chunks > 0);
        else assert(starts == 0 && chunks == 0 && interrupts == 0); /* Quiet, stale and impulse matches rejected. */
        ringbuf_free(); return;
    }
    assert(first_tx_frame == 20 && first_sample == 1 + offset && last_pre_sample == 2000 + offset);
    assert(starts == 2 && interrupts == 1 && flushes == 1 + server_break && mic_mutes == 0);
    assert(fresh_after_break == server_break);
    ringbuf_free();
}

int main(void)
{
    run_case(0, 0, 0);
    run_case(1, 0, 0);
    run_case(0, 37, 0);
    run_case(0, 0, 1);
    run_case(0, 0, 2);
    run_case(0, 0, 3);
    run_case(0, 0, 4);
    /* An XRUN/suspend invalidates the whole capture/reference epoch. Request
     * a full rebuild after all local audio, sender and KWS workers are stopped.
     * Other device errors still terminate rather than looping indefinitely. */
    for (device_operation = 1; device_operation <= 4; device_operation++) {
        device_error = BSP_ERR_AUDIO_DISCONTINUITY; expected_device_result = -3;
        run_case(0, 0, 0);
        device_error = -EINVAL; expected_device_result = -1;
        run_case(0, 0, 0);
    }
    puts("actual duplex loop: wake gate, pre-roll, immediate stop and continuous mic checks passed");
    return 0;
}
