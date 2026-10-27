/* V821 direct ALSA I/O. Device errors invalidate alignment and fail explicitly. */
#include "audio_pcm.h"
#include "board_config.h"
#include "bsp/board.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PCM_RATE 16000
#define PCM_PERIOD 320                 /* 20 ms */
#define PCM_BUFFER (PCM_PERIOD * 3)     /* request 60 ms */
#define PCM_MAX_BUFFER (PCM_RATE / 10)  /* reject >100 ms */
#define PCM_TIMEOUT_MS 2000

static snd_pcm_t *g_capture, *g_playback;
static snd_pcm_uframes_t g_playback_buffer;
static int g_capture_on, g_error;
static int64_t g_read_ms, g_write_ms, g_read_gap_ms, g_write_gap_ms, g_max_write_gap_ms;
static const int16_t g_silence[PCM_MAX_BUFFER];

static int64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int public_error(int error)
{
    if (error == -EPIPE) return BSP_ERR_AUDIO_DISCONTINUITY;
#ifdef ESTRPIPE
    if (error == -ESTRPIPE) return BSP_ERR_AUDIO_DISCONTINUITY;
#endif
    return error;
}

static int fail(const char *operation, int error)
{
    if (error >= 0) error = -EIO;
    if (!g_error) {
        fprintf(stderr, "[音频/ALSA] %s: %s; stopping instead of hiding a discontinuity\n",
                operation, snd_strerror(error));
        int64_t now = now_ms();
        fprintf(stderr, "[PCM诊断] rc=%d read_gap_ms=%lld write_gap_ms=%lld max_write_gap_ms=%lld since_read_ms=%lld since_write_ms=%lld\n",
                error, (long long)g_read_gap_ms, (long long)g_write_gap_ms,
                (long long)g_max_write_gap_ms,
                (long long)(g_read_ms && now >= g_read_ms ? now - g_read_ms : 0),
                (long long)(g_write_ms && now >= g_write_ms ? now - g_write_ms : 0));
    }
    g_error = public_error(error);
    return g_error;
}

static int configure(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     snd_pcm_uframes_t *actual_buffer)
{
    int rc;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
#define CHECK(call) do { rc = (call); if (rc < 0) return rc; } while (0)
    CHECK(snd_pcm_open(pcm, device, stream, SND_PCM_NONBLOCK));
    CHECK(snd_pcm_hw_params_any(*pcm, hw));
    CHECK(snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    CHECK(snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S16_LE));
    CHECK(snd_pcm_hw_params_set_channels(*pcm, hw, 1));
    CHECK(snd_pcm_hw_params_set_rate(*pcm, hw, PCM_RATE, 0));
    snd_pcm_uframes_t period = PCM_PERIOD, buffer = PCM_BUFFER;
    CHECK(snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, NULL));
    CHECK(snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buffer));
    CHECK(snd_pcm_hw_params(*pcm, hw));
    unsigned int rate;
    CHECK(snd_pcm_hw_params_get_rate(hw, &rate, NULL));
    CHECK(snd_pcm_hw_params_get_period_size(hw, &period, NULL));
    CHECK(snd_pcm_hw_params_get_buffer_size(hw, &buffer));
    if (rate != PCM_RATE || snd_pcm_hw_params_is_half_duplex(hw) ||
        buffer > PCM_MAX_BUFFER || buffer < PCM_PERIOD * 3 ||
        period == 0 || period > buffer / 2) return -EINVAL;
    CHECK(snd_pcm_sw_params_current(*pcm, sw));
    snd_pcm_uframes_t boundary;
    CHECK(snd_pcm_sw_params_get_boundary(sw, &boundary));
    /* Explicit start after priming; never let the final priming write auto-start. */
    CHECK(snd_pcm_sw_params_set_start_threshold(*pcm, sw, boundary));
    CHECK(snd_pcm_sw_params_set_avail_min(*pcm, sw, PCM_PERIOD));
    CHECK(snd_pcm_sw_params(*pcm, sw));
    CHECK(snd_pcm_prepare(*pcm));
    fprintf(stderr, "[音频/ALSA] %s device=%s rate=%u period=%lu buffer=%lu (%.1f ms)\n",
            stream == SND_PCM_STREAM_CAPTURE ? "capture" : "playback", device, rate,
            (unsigned long)period, (unsigned long)buffer, 1000.0 * buffer / PCM_RATE);
    *actual_buffer = buffer;
    return 0;
#undef CHECK
}

static int transfer(snd_pcm_t *pcm, int16_t *samples, int frames, int playback)
{
    int done = 0;
    int64_t start = now_ms();
    if (start < 0) return fail("monotonic clock", -EIO);
    while (done < frames) {
        int64_t now = now_ms();
        if (now < 0) return fail("monotonic clock", -EIO);
        if (now - start >= PCM_TIMEOUT_MS) return fail("I/O timeout", -ETIMEDOUT);
        snd_pcm_sframes_t n = playback
            ? snd_pcm_writei(pcm, samples + done, (snd_pcm_uframes_t)(frames - done))
            : snd_pcm_readi(pcm, samples + done, (snd_pcm_uframes_t)(frames - done));
        if (n > 0) {
            if (n > frames - done) return fail("invalid PCM transfer count", -EIO);
            done += (int)n;
            continue;
        }
        if (n == -EAGAIN || n == -EINTR || n == 0) {
            int rc = snd_pcm_wait(pcm, 100);
            if (rc >= 0 || rc == -EINTR) continue;
            return fail(playback ? "playback wait" : "capture wait", rc);
        }
        return fail(playback ? "playback transfer" : "capture transfer", (int)n);
    }
    return done;
}

static int prime_playback(snd_pcm_uframes_t frames)
{
    /* ALSA writei never modifies this buffer. The shared helper also serves readi. */
    int rc = transfer(g_playback, (int16_t *)g_silence, (int)frames, 1);
    return rc < 0 ? rc : 0;
}

static int restart_playback(int reserve_frame)
{
    int rc = snd_pcm_prepare(g_playback);
    if (rc < 0) return fail("playback prepare", rc);
    /* A server break can flush after capture but BEFORE this frame's write.
     * Filling the entire DAC queue would make that write wait another period,
     * while the ADC keeps recording. Keep one frame free for the caller, and
     * at least two frames of priming to survive a flush after a frame's write. */
    snd_pcm_uframes_t frames = g_playback_buffer - (reserve_frame ? PCM_PERIOD : 0);
    if ((rc = prime_playback(frames)) < 0) return rc;
    rc = snd_pcm_start(g_playback);
    return rc < 0 ? fail("playback start", rc) : 0;
}

int board_audio_start(int sample_rate)
{
    if (sample_rate != PCM_RATE) return -EINVAL;
    if (g_capture || g_playback) return -EBUSY;
    g_error = 0;
    const char *device = getenv("V821_ALSA_DEVICE");
    if (!device || !device[0]) device = BOARD_ALSA_CAPTURE_DEV;
    snd_pcm_uframes_t capture_buffer;
    int rc = configure(&g_capture, device, SND_PCM_STREAM_CAPTURE, &capture_buffer);
    if (rc < 0) { fail("capture configuration", rc); goto failed; }
    rc = configure(&g_playback, device, SND_PCM_STREAM_PLAYBACK, &g_playback_buffer);
    if (rc < 0) { fail("playback configuration", rc); goto failed; }
    if ((rc = prime_playback(g_playback_buffer)) < 0) goto failed;

    rc = snd_pcm_link(g_capture, g_playback);
    if (rc == 0) {
        rc = snd_pcm_start(g_capture);
        /* Link only for the initial start. Leaving it linked would let playback
         * flush stop capture too, destroying the always-listening guarantee. */
        int unlink_rc = snd_pcm_unlink(g_capture);
        if (rc < 0) { fail("linked start", rc); goto failed; }
        if (unlink_rc < 0) { rc = fail("unlink after start", unlink_rc); goto failed; }
        fprintf(stderr, "[音频/ALSA] linked start; streams unlinked for independent control\n");
    } else {
        fprintf(stderr, "[音频/ALSA] linked start unavailable (%s); sequential start, alignment must be measured\n",
                snd_strerror(rc));
        rc = snd_pcm_start(g_playback);
        if (rc < 0) { fail("playback start", rc); goto failed; }
        rc = snd_pcm_start(g_capture);
        if (rc < 0) { fail("capture start", rc); goto failed; }
    }
    g_capture_on = 1;
    return 0;
failed:
    board_audio_stop();
    return rc < 0 ? public_error(rc) : -EIO;
}

void board_audio_stop(void)
{
    if (g_capture) {
        snd_pcm_drop(g_capture);
        snd_pcm_close(g_capture);
        g_capture = NULL;
    }
    if (g_playback) {
        snd_pcm_drop(g_playback);
        snd_pcm_close(g_playback);
        g_playback = NULL;
    }
    g_capture_on = 0;
    g_playback_buffer = 0;
    g_error = 0;
    g_read_ms = g_write_ms = g_read_gap_ms = g_write_gap_ms = g_max_write_gap_ms = 0;
}

int board_audio_read(int16_t *mono, int frames)
{
    if (g_error) return g_error;
    if (!mono || frames <= 0 || frames > PCM_MAX_BUFFER) return -EINVAL;
    if (!g_capture) return -ENODEV;
    if (!g_capture_on) return 0;
    int rc = transfer(g_capture, mono, frames, 0);
    if (rc == frames) {
        int64_t now = now_ms();
        g_read_gap_ms = g_read_ms && now >= g_read_ms ? now - g_read_ms : 0;
        g_read_ms = now;
    }
    return rc;
}

int board_audio_write(const int16_t *mono, int frames)
{
    if (g_error) return g_error;
    if (!mono || frames <= 0 || frames > PCM_MAX_BUFFER) return -EINVAL;
    if (!g_playback) return -ENODEV;
    int rc = transfer(g_playback, (int16_t *)mono, frames, 1);
    if (rc == frames) {
        int64_t now = now_ms();
        g_write_gap_ms = g_write_ms && now >= g_write_ms ? now - g_write_ms : 0;
        if (g_write_gap_ms > g_max_write_gap_ms) g_max_write_gap_ms = g_write_gap_ms;
        g_write_ms = now;
    }
    return rc;
}

void board_audio_mic_set(int on)
{
    if (g_error || !g_capture || !!on == g_capture_on) return;
    int rc;
    if (!on) {
        rc = snd_pcm_drop(g_capture);
        if (rc < 0) { fail("capture pause", rc); return; }
        g_capture_on = 0;
        return;
    }
    rc = snd_pcm_prepare(g_capture);
    if (rc < 0) { fail("capture resume prepare", rc); return; }
    rc = snd_pcm_start(g_capture);
    if (rc < 0) { fail("capture resume start", rc); return; }
    g_capture_on = 1;
}

int board_audio_flush(void)
{
    if (g_error) return g_error;
    if (!g_playback) return -ENODEV;
    int rc = snd_pcm_drop(g_playback); /* discard queued speech before any wait */
    if (rc < 0) return fail("playback flush", rc);
    return restart_playback(1);
}

int board_audio_pending(void)
{
    if (g_error) return g_error;
    if (!g_playback) return -ENODEV;
    snd_pcm_sframes_t delay;
    int rc = snd_pcm_delay(g_playback, &delay);
    if (rc < 0) return fail("playback delay", rc);
    if (delay < 0) return fail("playback negative delay (underrun)", -EPIPE);
    if (delay > INT32_MAX) return fail("playback invalid delay", -EIO);
    return (int)delay;
}

void board_audio_drain(void)
{
    if (g_error || !g_playback) return;
    int64_t start = now_ms();
    if (start < 0) { fail("monotonic clock", -EIO); return; }
    for (;;) {
        int rc = snd_pcm_drain(g_playback);
        if (rc == 0) break;
        if (rc != -EAGAIN && rc != -EINTR) { fail("playback drain", rc); return; }
        int64_t now = now_ms();
        if (now < 0) { fail("monotonic clock", -EIO); return; }
        if (now - start >= PCM_TIMEOUT_MS) { fail("playback drain timeout", -ETIMEDOUT); return; }
        rc = snd_pcm_wait(g_playback, 100);
        if (rc < 0 && rc != -EINTR) { fail("playback drain wait", rc); return; }
    }
    restart_playback(0);
}
