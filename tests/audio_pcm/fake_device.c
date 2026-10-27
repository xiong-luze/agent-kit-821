/* Device double implements only the ALSA calls made by this BSP. These are real
 * ALSA declarations; this is not a replacement header or a target-runtime test. */
#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "audio_pcm.h"
#include "bsp/board.h"

struct _snd_pcm {
    int stream, running, drops, starts, closes;
    snd_pcm_sframes_t queued;
    int samples_read;
};
struct _snd_pcm_hw_params {
    unsigned int rate;
    snd_pcm_uframes_t period, buffer;
};
struct _snd_pcm_sw_params { snd_pcm_uframes_t boundary; };

static struct _snd_pcm capture, playback;
static int linked, partial, read_step, write_step, xrun, timeout_read, drain_calls, delay_error;
static int fail_open = -1, override_rate, override_buffer, half_duplex;
static int16_t written[4096];
static int written_count;
static int clocked;
static void advance_clock(snd_pcm_sframes_t frames)
{
    if (capture.running) capture.queued += frames;
    if (playback.running) {
        playback.queued -= frames;
        if (playback.queued < 0) playback.queued = 0;
    }
}

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

size_t snd_pcm_hw_params_sizeof(void) { return sizeof(struct _snd_pcm_hw_params); }
size_t snd_pcm_sw_params_sizeof(void) { return sizeof(struct _snd_pcm_sw_params); }
const char *snd_strerror(int error) { (void)error; return "fake device error"; }

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
    (void)name; (void)mode;
    if ((int)stream == fail_open) return -EBUSY;
    *pcm = stream == SND_PCM_STREAM_CAPTURE ? &capture : &playback;
    (*pcm)->stream = stream;
    return 0;
}

int snd_pcm_close(snd_pcm_t *pcm) { pcm->closes++; return 0; }
int snd_pcm_drop(snd_pcm_t *pcm)
{
    pcm->drops++;
    pcm->running = 0;
    pcm->queued = 0;
    if (linked) { capture.running = playback.running = 0; }
    return 0;
}
int snd_pcm_prepare(snd_pcm_t *pcm) { pcm->queued = 0; pcm->running = 0; return 0; }
int snd_pcm_start(snd_pcm_t *pcm)
{
    pcm->starts++;
    pcm->running = 1;
    if (linked) capture.running = playback.running = 1;
    return 0;
}
int snd_pcm_link(snd_pcm_t *first, snd_pcm_t *second)
{ (void)first; (void)second; linked = 1; return 0; }
int snd_pcm_unlink(snd_pcm_t *pcm) { (void)pcm; linked = 0; return 0; }
int snd_pcm_delay(snd_pcm_t *pcm, snd_pcm_sframes_t *delay)
{ if (delay_error) return delay_error; *delay = pcm->queued; return 0; }
int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
    (void)pcm; (void)timeout;
    if (timeout_read) { struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL); return 0; }
    if (clocked) advance_clock(320);
    return 1;
}
int snd_pcm_drain(snd_pcm_t *pcm)
{
    if (++drain_calls == 1) return -EAGAIN;
    pcm->running = 0;
    pcm->queued = 0;
    return 0;
}
snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t frames)
{
    if (xrun) return -EPIPE;
    if (timeout_read) return -EAGAIN;
    if (!pcm->running) return -EBADF;
    if (partial && read_step++ == 0) return -EINTR;
    if (partial && frames > 7) frames = 7;
    int16_t *out = buffer;
    for (snd_pcm_uframes_t i = 0; i < frames; i++) out[i] = (int16_t)(++pcm->samples_read);
    if (clocked) {
        if (pcm->queued < (snd_pcm_sframes_t)frames)
            advance_clock((snd_pcm_sframes_t)frames - pcm->queued);
        pcm->queued -= (snd_pcm_sframes_t)frames;
    } else {
        playback.queued -= (snd_pcm_sframes_t)frames;
        if (playback.queued < 0) playback.queued = 0;
    }
    return (snd_pcm_sframes_t)frames;
}
snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t frames)
{
    if (xrun) return -EPIPE;
    if (clocked && pcm->running) {
        if (pcm->queued >= 960) return -EAGAIN;
        if (frames > (snd_pcm_uframes_t)(960 - pcm->queued))
            frames = (snd_pcm_uframes_t)(960 - pcm->queued);
    }
    if (partial && write_step++ == 0) return -EAGAIN;
    if (partial && frames > 5) frames = 5;
    const int16_t *input = buffer;
    for (snd_pcm_uframes_t i = 0; i < frames; i++) {
        if (written_count >= (int)(sizeof(written) / sizeof(written[0]))) return -EIO;
        written[written_count++] = input[i];
    }
    pcm->queued += (snd_pcm_sframes_t)frames;
    return (snd_pcm_sframes_t)frames;
}

int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw)
{ (void)pcm; memset(hw, 0, sizeof(*hw)); return 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, snd_pcm_access_t access)
{ (void)pcm; (void)hw; return access == SND_PCM_ACCESS_RW_INTERLEAVED ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, snd_pcm_format_t format)
{ (void)pcm; (void)hw; return format == SND_PCM_FORMAT_S16_LE ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_channels(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, unsigned int channels)
{ (void)pcm; (void)hw; return channels == 1 ? 0 : -EINVAL; }
int snd_pcm_hw_params_set_rate(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw, unsigned int rate, int direction)
{ (void)pcm; (void)direction; hw->rate = override_rate ? (unsigned)override_rate : rate; return 0; }
int snd_pcm_hw_params_set_period_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                          snd_pcm_uframes_t *period, int *direction)
{ (void)pcm; (void)direction; hw->period = *period; return 0; }
int snd_pcm_hw_params_set_buffer_size_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw,
                                          snd_pcm_uframes_t *buffer)
{ (void)pcm; hw->buffer = override_buffer ? (snd_pcm_uframes_t)override_buffer : *buffer; return 0; }
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *hw)
{ (void)pcm; (void)hw; return 0; }
int snd_pcm_hw_params_get_rate(const snd_pcm_hw_params_t *hw, unsigned int *rate, int *direction)
{ (void)direction; *rate = hw->rate; return 0; }
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *hw,
                                     snd_pcm_uframes_t *period, int *direction)
{ (void)direction; *period = hw->period; return 0; }
int snd_pcm_hw_params_get_buffer_size(const snd_pcm_hw_params_t *hw, snd_pcm_uframes_t *buffer)
{ *buffer = hw->buffer; return 0; }
int snd_pcm_hw_params_is_half_duplex(const snd_pcm_hw_params_t *hw)
{ (void)hw; return half_duplex; }
int snd_pcm_sw_params_current(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw)
{ (void)pcm; sw->boundary = 1u << 30; return 0; }
int snd_pcm_sw_params_get_boundary(const snd_pcm_sw_params_t *sw, snd_pcm_uframes_t *boundary)
{ *boundary = sw->boundary; return 0; }
int snd_pcm_sw_params_set_start_threshold(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw,
                                         snd_pcm_uframes_t threshold)
{ (void)pcm; return threshold == sw->boundary ? 0 : -EINVAL; }
int snd_pcm_sw_params_set_avail_min(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw,
                                   snd_pcm_uframes_t minimum)
{ (void)pcm; (void)sw; return minimum == 320 ? 0 : -EINVAL; }
int snd_pcm_sw_params(snd_pcm_t *pcm, snd_pcm_sw_params_t *sw)
{ (void)pcm; (void)sw; return 0; }

static int test_partial(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    partial = 1;
    written_count = 0;
    int16_t samples[32];
    REQUIRE(board_audio_read(samples, 32) == 32);
    for (int i = 0; i < 32; i++) REQUIRE(samples[i] == i + 1);
    REQUIRE(board_audio_write(samples, 32) == 32);
    REQUIRE(written_count == 32);
    for (int i = 0; i < 32; i++) REQUIRE(written[i] == i + 1);
    board_audio_stop();
    return 0;
}

static int test_flush(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    REQUIRE(!linked);
    int capture_starts = capture.starts, capture_drops = capture.drops;
    int playback_drops = playback.drops;
    written_count = 0;
    REQUIRE(board_audio_flush() == 0);
    REQUIRE(capture.running && playback.running);
    REQUIRE(capture.starts == capture_starts && capture.drops == capture_drops);
    REQUIRE(playback.drops == playback_drops + 1);
    REQUIRE(board_audio_pending() == 640);
    for (int i = 0; i < written_count; i++) REQUIRE(written[i] == 0);
    board_audio_stop();
    return 0;
}

static int test_xrun(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    int16_t samples[32] = {0};
    xrun = 1;
    REQUIRE(board_audio_read(samples, 32) == BSP_ERR_AUDIO_DISCONTINUITY);
    xrun = 0;
    REQUIRE(board_audio_write(samples, 32) == BSP_ERR_AUDIO_DISCONTINUITY);
    REQUIRE(board_audio_flush() == BSP_ERR_AUDIO_DISCONTINUITY);
    board_audio_stop();
    written_count = 0;
    REQUIRE(board_audio_start(16000) == 0);
    REQUIRE(board_audio_read(samples, 32) == 32);
    int starts_before = playback.starts;
    delay_error = -EPIPE;
    REQUIRE(board_audio_pending() == BSP_ERR_AUDIO_DISCONTINUITY);
    delay_error = 0;
    REQUIRE(board_audio_pending() == BSP_ERR_AUDIO_DISCONTINUITY);
    REQUIRE(playback.starts == starts_before); /* No hidden one-stream restart. */
    board_audio_stop();
    return 0;
}

static int test_flush_cadence(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    clocked = 1;
    int16_t samples[320] = {0};
    for (int frame = 0; frame < 50; frame++) {
        written_count = 0;
        REQUIRE(board_audio_read(samples, 320) == 320);
        if (frame % 5 == 0) REQUIRE(board_audio_flush() == 0);
        REQUIRE(board_audio_write(samples, 320) == 320);
        /* A pre-write flush must leave room for this frame. Waiting for DAC
         * space here advances the still-running ADC and consumes its margin. */
        REQUIRE(capture.queued == 0);
    }
    board_audio_stop();
    return 0;
}

static int test_mic(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    int playback_drops = playback.drops;
    board_audio_mic_set(0);
    int16_t samples[32];
    REQUIRE(board_audio_read(samples, 32) == 0);
    REQUIRE(!capture.running && playback.running);
    board_audio_mic_set(1);
    REQUIRE(capture.running && playback.running);
    REQUIRE(playback.drops == playback_drops);
    REQUIRE(board_audio_read(samples, 32) == 32);
    board_audio_stop();
    return 0;
}

static int test_configuration(void)
{
    REQUIRE(board_audio_start(8000) == -EINVAL);
    override_rate = 48000;
    REQUIRE(board_audio_start(16000) == -EINVAL);
    REQUIRE(capture.closes == 1 && playback.closes == 0);
    override_rate = 0;
    override_buffer = 1601;
    REQUIRE(board_audio_start(16000) == -EINVAL);
    override_buffer = 640;
    REQUIRE(board_audio_start(16000) == -EINVAL);
    override_buffer = 0;
    half_duplex = 1;
    REQUIRE(board_audio_start(16000) == -EINVAL);
    half_duplex = 0;
    REQUIRE(board_audio_start(16000) == 0);
    REQUIRE(board_audio_start(16000) == -EBUSY);
    REQUIRE(capture.running && playback.running);
    board_audio_stop();
    return 0;
}

static int test_open_failure(void)
{
    fail_open = SND_PCM_STREAM_CAPTURE;
    REQUIRE(board_audio_start(16000) == -EBUSY);
    REQUIRE(capture.closes == 0 && playback.closes == 0);
    fail_open = SND_PCM_STREAM_PLAYBACK;
    REQUIRE(board_audio_start(16000) == -EBUSY);
    REQUIRE(capture.closes == 1 && playback.closes == 0);
    fail_open = -1;
    REQUIRE(board_audio_start(16000) == 0);
    board_audio_stop();
    return 0;
}

static int test_timeout(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    timeout_read = 1;
    int16_t samples[32];
    REQUIRE(board_audio_read(samples, 32) == -ETIMEDOUT);
    REQUIRE(board_audio_pending() == -ETIMEDOUT);
    board_audio_stop();
    return 0;
}

static int test_drain(void)
{
    REQUIRE(board_audio_start(16000) == 0);
    board_audio_mic_set(0);
    board_audio_drain();
    REQUIRE(drain_calls == 2 && playback.running && !capture.running);
    REQUIRE(board_audio_pending() == 960);
    board_audio_mic_set(1);
    REQUIRE(capture.running);
    board_audio_stop();
    return 0;
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    if (!strcmp(argv[1], "partial")) return test_partial();
    if (!strcmp(argv[1], "flush")) return test_flush();
    if (!strcmp(argv[1], "flush-cadence")) return test_flush_cadence();
    if (!strcmp(argv[1], "xrun")) return test_xrun();
    if (!strcmp(argv[1], "mic")) return test_mic();
    if (!strcmp(argv[1], "configuration")) return test_configuration();
    if (!strcmp(argv[1], "open-failure")) return test_open_failure();
    if (!strcmp(argv[1], "timeout")) return test_timeout();
    if (!strcmp(argv[1], "drain")) return test_drain();
    return 2;
}
