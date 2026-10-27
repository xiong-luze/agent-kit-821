/* Uses the actual application DSP and dialog controller. Synthesized echo is
 * a reproducible software boundary check, not device acoustic acceptance. */
#include "core/audio_dsp.h"
#include "core/dialog.h"
#include "core/echo_guard.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE 16000
#define FRAME 320

static double guard_total_us, guard_max_us;
static unsigned guard_calls;

static double microseconds(void)
{
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec * 1000000.0 + t.tv_nsec / 1000.0;
}

static int run_guard(echo_guard_t *guard, const int16_t *reference,
                      const int16_t *residual, size_t samples, echo_guard_result_t *result)
{
    double start = microseconds();
    int rc = echo_guard_process(guard, reference, residual, samples, result);
    double elapsed = microseconds() - start;
    guard_calls++;
    guard_total_us += elapsed;
    if (elapsed > guard_max_us) guard_max_us = elapsed;
    return rc;
}

static int16_t *load(const char *path, size_t *count)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long bytes = ftell(file);
    assert(bytes > 0 && bytes % 2 == 0);
    rewind(file);
    int16_t *pcm = malloc((size_t)bytes);
    assert(pcm && fread(pcm, 1, (size_t)bytes, file) == (size_t)bytes);
    assert(fclose(file) == 0);
    *count = (size_t)bytes / 2;
    return pcm;
}

static int16_t clamp(int value, int limit)
{
    if (value > limit) value = limit;
    if (value < -limit) value = -limit;
    return (int16_t)value;
}

static dialog_config_t config(int threshold)
{
    dialog_config_t c = {.frame_ms = 20, .wake_enabled = 0, .wake_hold_ms = 30000,
        .vad_start = threshold, .vad_end = threshold / 2, .vad_hang_ms = 1000,
        .barge_ms = 120, .max_turn_ms = 15000, .reply_timeout_ms = 30000,
        .tail_quiet_ms = 800};
    return c;
}

static void near_only_is_allowed(void)
{
    echo_guard_t guard;
    echo_guard_init(&guard);
    int16_t ref[FRAME] = {0}, residual[FRAME];
    uint32_t seed = 17;
    for (int frame = 0; frame < 8; frame++) {
        for (int i = 0; i < FRAME; i++) {
            seed = seed * 1664525u + 1013904223u;
            residual[i] = (int16_t)((int)(seed >> 20) - 2048);
        }
        echo_guard_result_t result;
        assert(run_guard(&guard, ref, residual, FRAME, &result) == 0);
        assert(result.near_allowed && !result.reference_active);
    }
    echo_guard_result_t result;
    assert(echo_guard_process(&guard, ref, residual, 319, &result) != 0);
    assert(echo_guard_process(NULL, ref, residual, FRAME, &result) != 0);
    /* Independent near-end evidence is allowed even in the first frame of a
     * reference-active session. There is no blind elapsed-time startup mute. */
    echo_guard_init(&guard);
    uint32_t other = 391;
    for (int frame = 0; frame < 2; frame++) {
        for (int i = 0; i < FRAME; i++) {
            seed = seed * 1664525u + 1013904223u;
            other = other * 1664525u + 1013904223u;
            ref[i] = (int16_t)((int)(seed >> 20) - 2048);
            residual[i] = (int16_t)((int)(other >> 20) - 2048);
        }
        assert(run_guard(&guard, ref, residual, FRAME, &result) == 0);
        assert(result.near_allowed && result.reference_active);
    }
}

static void causal_delay_checks(void)
{
    int16_t signal[RATE * 2];
    uint32_t seed = 31;
    for (size_t i = 0; i < sizeof(signal) / sizeof(signal[0]); i++) {
        seed = seed * 1664525u + 1013904223u;
        signal[i] = (int16_t)((int)(seed >> 19) - 4096);
    }
    const int delays[] = {0, 8, 952, 960, 1272, 3200, 4800};
    for (size_t test = 0; test < sizeof(delays) / sizeof(delays[0]); test++) {
        echo_guard_t guard;
        echo_guard_init(&guard);
        for (int index = 0; index + FRAME < RATE * 2; index += FRAME) {
            int16_t residual[FRAME];
            for (int i = 0; i < FRAME; i++) {
                int at = index + i - delays[test];
                residual[i] = at >= 0 ? (int16_t)(-signal[at] / 2) : 0;
                if (at >= 80) residual[i] += signal[at - 80] / 10;
            }
            echo_guard_result_t result;
            assert(run_guard(&guard, signal + index, residual, FRAME, &result) == 0);
            if (index >= delays[test] + RATE / 5) {
                if (result.near_allowed || result.correlation < 0.95f)
                    fprintf(stderr, "missed causal delay=%d samples correlation=%.3f\n",
                            delays[test], result.correlation);
                assert(!result.near_allowed && result.correlation >= 0.95f);
            }
        }
    }
}

static void actual_dsp_case(const int16_t *input, size_t samples,
                            const int16_t *near, size_t near_samples,
                            int threshold, int use_guard, int delay, int multipath)
{
    int16_t *far = malloc(samples * sizeof(*far));
    assert(far);
    for (size_t i = 0; i < samples; i++) far[i] = clamp(input[i] * 2, 24000);
    assert(dsp_init(RATE, FRAME, 300, 1) == 0);
    int16_t zero[FRAME] = {0}, out[FRAME];
    for (int i = 0; i < 100; i++) assert(dsp_process(zero, zero, out) == 0);
    assert(dsp_reset() == 0); /* Exactly the discontinuity previously exposing false barge. */
    echo_guard_t guard;
    echo_guard_init(&guard);
    dialog_config_t cfg = config(threshold);
    dialog_t dialog;
    assert(dialog_init(&dialog, &cfg, 0) == 0);
    dialog.state = DIALOG_WAIT;
    int triggered = -1, peak = 0, clips = 0, blocked_loud_frames = 0;
    float max_correlation = 0;
    for (size_t index = 0; index + FRAME <= samples; index += FRAME) {
        int16_t mic[FRAME], ref[FRAME];
        for (int i = 0; i < FRAME; i++) {
            size_t sample = index + (size_t)i;
            ref[i] = far[sample];
            int value = sample >= (size_t)delay ? (int)(far[sample - delay] * 0.7) : 0;
            if (multipath && sample >= (size_t)delay + 80)
                value += (int)(far[sample - delay - 80] * 0.15);
            /* Begin an independently spoken command at t=1s, discarding the
             * fixture's 0.5s leading silence rather than delaying it again. */
            if (near && sample >= RATE && sample - RATE + RATE / 2 < near_samples)
                value += near[sample - RATE + RATE / 2];
            clips += value > 32767 || value < -32768;
            mic[i] = clamp(value, 32767);
        }
        assert(dsp_process(mic, ref, out) == 0);
        echo_guard_result_t result;
        assert(run_guard(&guard, ref, out, FRAME, &result) == 0);
        int rms = dsp_rms(out, FRAME);
        if (rms > peak) peak = rms;
        if (result.correlation > max_correlation) max_correlation = result.correlation;
        if (!result.near_allowed && rms >= threshold + threshold / 2) blocked_loud_frames++;
        dialog_input_t in = {.now_ms = (int64_t)index * 1000 / RATE,
            .rms = !use_guard || result.near_allowed ? rms : 0,
            .reply_audio = 1, .playback_active = 1};
        if (triggered < 0 && (dialog_step(&dialog, &in) & DIALOG_INTERRUPT))
            triggered = (int)in.now_ms;
    }
    assert(clips == 0);
    printf("actual-DSP %s guard=%d threshold=%d delay_samples=%d multipath=%d trigger_ms=%d residual_peak=%d "
           "correlation_max=%.3f blocked_loud=%d\n", near ? "double-talk" : "far-only",
           use_guard, threshold, delay, multipath, triggered, peak, max_correlation, blocked_loud_frames);
    if (!near && !use_guard) assert(triggered >= 0); /* The historical regression exists. */
    else if (!near) assert(triggered < 0 && blocked_loud_frames > 0);
    else assert(triggered >= 1000 && triggered <= 1260);
    dsp_deinit();
    free(far);
}

int main(int argc, char **argv)
{
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s [FAR_PCM16K NEAR_PCM16K]\n", argv[0]);
        return 1;
    }
    near_only_is_allowed();
    causal_delay_checks();
    if (argc == 3) {
        size_t far_samples, near_samples;
        int16_t *far = load(argv[1], &far_samples), *near = load(argv[2], &near_samples);
        actual_dsp_case(far, far_samples, NULL, 0, 1000, 0, 960, 0);
        const int thresholds[] = {300, 500, 1000, 1500};
        for (size_t i = 0; i < sizeof(thresholds) / sizeof(thresholds[0]); i++)
            actual_dsp_case(far, far_samples, NULL, 0, thresholds[i], 1, 960, 0);
        actual_dsp_case(far, far_samples, NULL, 0, 1000, 1, 1920, 1);
        actual_dsp_case(far, far_samples, near, near_samples, 1000, 1, 960, 0);
        free(far); free(near);
    }
    printf("guard state=%zu bytes calls=%u mean_us=%.1f max_us=%.1f (host only)\n",
           sizeof(echo_guard_t), guard_calls, guard_total_us / guard_calls, guard_max_us);
    puts("reference/residual guard checks passed; hardware quality and budget remain unverified");
    return 0;
}
