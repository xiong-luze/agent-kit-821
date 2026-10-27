/* Actual legacy app loop on a continuously-running 60 ms DAC. The clock and
 * device are deterministic doubles; ring consumption and app logic are real.
 * This catches silence starvation in MCP gaps/tail waits and post-drain guard. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int fake_clock_gettime(clockid_t clock_id, struct timespec *out);
static int fake_nanosleep(const struct timespec *duration, struct timespec *remaining);
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
#include "../core/app.c"
#undef clock_gettime
#undef nanosleep

enum { FRAME_SAMPLES = 320, BUFFER_SAMPLES = 960, SAMPLES_PER_MS = 16 };
static int64_t virtual_ms, dac_samples, end_ms, first_ms, drained_ms, resumed_ms;
static session_state_t cloud;
static int capture_on, phase, starved, mic_violation, gaps_case;
static int first_injected, second_injected, mic_stops, mic_resumes, drain_calls;
static int chunks_sent, speech_writes, gap_zero_writes, guard_zero_writes;
static const char *case_name;

static void advance_ms(int milliseconds)
{
    virtual_ms += milliseconds;
    dac_samples -= (int64_t)milliseconds * SAMPLES_PER_MS;
    if (dac_samples < 0 && !starved) {
        starved = phase;
        fprintf(stderr, "%s: DAC starvation during %s at %lld ms\n", case_name,
                phase == 2 ? "post-drain guard" : "reply gap/tail wait", (long long)virtual_ms);
        cloud.lost = 1;
    }
    if (virtual_ms > 40000) cloud.lost = 1; /* Fixture cannot hang on a regression. */
}

static int fake_clock_gettime(clockid_t clock_id, struct timespec *out)
{
    (void)clock_id;
    out->tv_sec = (time_t)(virtual_ms / 1000);
    out->tv_nsec = (long)(virtual_ms % 1000) * 1000000L;
    return 0;
}

static int fake_nanosleep(const struct timespec *duration, struct timespec *remaining)
{
    advance_ms((int)(duration->tv_sec * 1000 + duration->tv_nsec / 1000000));
    if (remaining) memset(remaining, 0, sizeof(*remaining));
    return 0;
}

static int fake_read(int16_t *out, int frames)
{
    if (!capture_on) { mic_violation = 1; return 0; }
    advance_ms(frames / SAMPLES_PER_MS);
    if (starved) return -EPIPE;
    for (int i = 0; i < frames; i++) out[i] = end_ms ? 0 : 2000;
    return frames;
}

static int fake_write(const int16_t *pcm, int frames)
{
    if (starved) return -EPIPE;
    /* A prepared/running ALSA ring cannot accept more than its bounded size.
     * Waiting for space consumes queued audio and paces the caller. */
    if (dac_samples + frames > BUFFER_SAMPLES)
        advance_ms((int)((dac_samples + frames - BUFFER_SAMPLES + SAMPLES_PER_MS - 1) /
                         SAMPLES_PER_MS));
    if (starved) return -EPIPE;
    dac_samples += frames;
    int nonzero = 0;
    for (int i = 0; i < frames; i++) nonzero |= pcm[i] != 0;
    if (phase == 1 || phase == 2) {
        if (capture_on) mic_violation = 1;
        if (nonzero) speech_writes++;
        else if (phase == 1) gap_zero_writes++;
        else guard_zero_writes++;
    }
    return frames;
}

static void fake_mic_set(int on)
{
    if (!on) {
        capture_on = 0;
        phase = 1;
        mic_stops++;
    } else {
        if (!drained_ms || virtual_ms - drained_ms < 300) mic_violation = 1;
        capture_on = 1;
        phase = 3;
        resumed_ms = virtual_ms;
        mic_resumes++;
        cloud.lost = 1; /* End the fixture after the first completed reply. */
    }
}

static void fake_drain(void)
{
    drain_calls++;
    if (starved) return;
    /* Deliberate drain reaching exactly zero is not an XRUN. The real direct
     * backend then prepares, primes silence, and restarts the DAC. */
    advance_ms((int)(dac_samples / SAMPLES_PER_MS));
    dac_samples = BUFFER_SAMPLES;
    drained_ms = virtual_ms;
    phase = 2;
}

static const bsp_t board = {
    .name = "half-duplex on continuous PCM", .caps = BSP_CAP_AUDIO | BSP_CAP_AUDIO_DUPLEX,
    .audio_read = fake_read, .audio_write = fake_write,
    .audio_mic_set = fake_mic_set, .audio_drain = fake_drain,
};
const bsp_t *bsp_get(void) { return &board; }

void session_snapshot(session_state_t *out)
{
    if (!cloud.lost && end_ms && !first_injected) {
        int16_t pcm[FRAME_SAMPLES];
        for (int i = 0; i < FRAME_SAMPLES; i++) pcm[i] = 100;
        if (ringbuf_push(pcm, FRAME_SAMPLES) != FRAME_SAMPLES) cloud.lost = 1;
        first_injected = 1;
        first_ms = virtual_ms;
        cloud.got_audio = cloud.turn_done = 1;
        /* For the guard fixture, queued audio is already older than the tail
         * criterion. This avoids the earlier gap failure masking the guard. */
        cloud.last_audio_ms = gaps_case ? virtual_ms : virtual_ms - 1000;
    }
    if (!cloud.lost && gaps_case && first_injected && !second_injected &&
        virtual_ms - first_ms >= 200) {
        int16_t pcm[FRAME_SAMPLES];
        for (int i = 0; i < FRAME_SAMPLES; i++) pcm[i] = 200;
        if (ringbuf_push(pcm, FRAME_SAMPLES) != FRAME_SAMPLES) cloud.lost = 1;
        second_injected = 1;
        cloud.turn_done = 1;
        cloud.last_audio_ms = virtual_ms;
    }
    *out = cloud;
}

void session_turn_begin(void)
{
    cloud.got_audio = cloud.turn_done = cloud.asr_final = 0;
}
int session_audio_start(void) { return 0; }
int session_audio_chunk(const int16_t *pcm, int frames)
{
    (void)pcm; (void)frames;
    if (++chunks_sent >= 2) cloud.asr_final = 1;
    return 0;
}
int session_audio_end(void) { end_ms = virtual_ms; return 0; }
int session_send_text(const char *text) { (void)text; return -1; }

int dsp_process(const int16_t *mic, const int16_t *ref, int16_t *out)
{ (void)ref; memcpy(out, mic, FRAME_SAMPLES * sizeof(*out)); return 0; }
int dsp_rms(const int16_t *pcm, int count) { (void)count; return pcm[0] < 0 ? -pcm[0] : pcm[0]; }
void vad_cal_begin(void) {}
void vad_cal_feed(int value) { (void)value; }
vad_thresh_t vad_cal_end(int start_mul10, int end_mul10)
{ (void)start_mul10; (void)end_mul10; vad_thresh_t t = {1000, 600}; return t; }
void presence_cfg_defaults(presence_cfg_t *config) { memset(config, 0, sizeof(*config)); }
int presence_start(const presence_cfg_t *config) { (void)config; return -1; }
void presence_stop(void) {}
void presence_set_enabled(int enabled) { (void)enabled; }
void presence_rearm(void) {}
int presence_take_trigger(void) { return 0; }
void ui_set_status(const char *text) { (void)text; }
void ui_set_user(const char *text) { (void)text; }
void ui_ai_begin(void) {}

static int run_case(int use_gap)
{
    memset(&cloud, 0, sizeof(cloud));
    virtual_ms = 10000;
    dac_samples = BUFFER_SAMPLES;
    capture_on = 1;
    phase = starved = mic_violation = 0;
    first_injected = second_injected = mic_stops = mic_resumes = drain_calls = 0;
    chunks_sent = speech_writes = gap_zero_writes = guard_zero_writes = 0;
    end_ms = first_ms = drained_ms = resumed_ms = 0;
    gaps_case = use_gap;
    case_name = use_gap ? "MCP gap/tail" : "guard after drain";
    if (ringbuf_init(16000, 4)) return 1;
    app_cfg_t config = {
        .sample_rate = 16000, .frame_ms = 20, .vad_start = 1000, .vad_end = 600,
        .vad_hang_ms = 200, .guard_ms = 300, .voice_trigger = 1,
    };
    app_run(&config);
    ringbuf_free();
    int failed = starved || mic_violation || mic_stops != 1 || mic_resumes != 1 ||
        drain_calls != 1 || !drained_ms || resumed_ms - drained_ms < 300 ||
        guard_zero_writes == 0 || speech_writes != (use_gap ? 2 : 1) ||
        (use_gap && (!second_injected || gap_zero_writes == 0));
    if (failed)
        fprintf(stderr, "%s failed: starved_phase=%d mic_violation=%d mute/resume=%d/%d "
                "drain=%d speech=%d gap_zeros=%d guard_zeros=%d guard_ms=%lld\n",
                case_name, starved, mic_violation, mic_stops, mic_resumes, drain_calls,
                speech_writes, gap_zero_writes, guard_zero_writes,
                (long long)(resumed_ms - drained_ms));
    else
        printf("%s: continuous silence and capture mute/resume verified\n", case_name);
    return failed;
}

int main(void)
{
    int failures = run_case(1);
    failures += run_case(0);
    return failures ? 1 : 0;
}
