/* Continuous, bounded-buffer ALSA experiment. Any XRUN invalidates the run. */
#include "probe.h"

#include <alloca.h>
#include <alsa/asoundlib.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stopped;

static void stop_recording(int sig)
{
    (void)sig;
    stopped = 1;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int setup_pcm(snd_pcm_t **pcm, const char *device, snd_pcm_stream_t stream,
                     snd_pcm_uframes_t *actual_buffer)
{
    int rc;
    const char *name = stream == SND_PCM_STREAM_PLAYBACK ? "playback" : "capture";
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);
#define CHECK(call) do { rc = (call); if (rc < 0) goto fail; } while (0)
    CHECK(snd_pcm_open(pcm, device, stream, SND_PCM_NONBLOCK));
    CHECK(snd_pcm_hw_params_any(*pcm, hw));
    CHECK(snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED));
    CHECK(snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S16_LE));
    CHECK(snd_pcm_hw_params_set_channels(*pcm, hw, 1));
    CHECK(snd_pcm_hw_params_set_rate(*pcm, hw, PROBE_RATE, 0));
    snd_pcm_uframes_t period = PROBE_FRAME, buffer = PROBE_FRAME * 4;
    CHECK(snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period, NULL));
    CHECK(snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buffer));
    CHECK(snd_pcm_hw_params(*pcm, hw));
    CHECK(snd_pcm_hw_params_get_period_size(hw, &period, NULL));
    CHECK(snd_pcm_hw_params_get_buffer_size(hw, &buffer));
    if (snd_pcm_hw_params_is_half_duplex(hw) || buffer > PROBE_RATE / 10 ||
        buffer < PROBE_FRAME * 2 || period > buffer / 2) {
        fprintf(stderr, "%s requires full duplex and 20..100 ms buffer with >=2 periods\n", name);
        return -1;
    }
    CHECK(snd_pcm_sw_params_current(*pcm, sw));
    snd_pcm_uframes_t boundary;
    CHECK(snd_pcm_sw_params_get_boundary(sw, &boundary));
    /* Explicit start after both streams are prepared and playback is primed. */
    CHECK(snd_pcm_sw_params_set_start_threshold(*pcm, sw, boundary));
    CHECK(snd_pcm_sw_params_set_avail_min(*pcm, sw, PROBE_FRAME));
    CHECK(snd_pcm_sw_params(*pcm, sw));
    CHECK(snd_pcm_prepare(*pcm));
    fprintf(stderr, "%s device=%s rate=%d period=%lu buffer=%lu (%.1f ms)\n",
            name, device, PROBE_RATE, (unsigned long)period, (unsigned long)buffer,
            1000.0 * buffer / PROBE_RATE);
    *actual_buffer = buffer;
    return 0;
fail:
    fprintf(stderr, "%s ALSA configuration failed: %s\n", name, snd_strerror(rc));
    return -1;
#undef CHECK
}

static int transfer(snd_pcm_t *pcm, int16_t *samples, snd_pcm_uframes_t count, int playback)
{
    snd_pcm_uframes_t done = 0;
    int64_t deadline = monotonic_ms() + 2000;
    while (done < count && !stopped) {
        snd_pcm_sframes_t n = playback ? snd_pcm_writei(pcm, samples + done, count - done)
                                      : snd_pcm_readi(pcm, samples + done, count - done);
        if (n > 0) done += (snd_pcm_uframes_t)n;
        else if (n == -EAGAIN || n == -EINTR || n == 0) {
            if (monotonic_ms() >= deadline) {
                fprintf(stderr, "ALSA %s stalled for 2 seconds\n", playback ? "playback" : "capture");
                return -1;
            }
            int rc = snd_pcm_wait(pcm, 100);
            if (rc < 0 && rc != -EINTR) n = rc;
            else continue;
        }
        if (n < 0) {
            fprintf(stderr, "ALSA %s failed: %s; aborting instead of hiding a discontinuity\n",
                    playback ? "playback" : "capture", snd_strerror((int)n));
            return -1;
        }
    }
    return done == count ? 0 : -1;
}

static FILE *output_file(const char *prefix, const char *suffix)
{
    char path[1024];
    int size = snprintf(path, sizeof(path), "%s%s", prefix, suffix);
    if (size < 0 || (size_t)size >= sizeof(path)) {
        fprintf(stderr, "output prefix is too long\n");
        return NULL;
    }
    return probe_new_file(path);
}

static int clipping(const int16_t *pcm)
{
    int count = 0;
    for (int i = 0; i < PROBE_FRAME; i++)
        count += pcm[i] >= 32760 || pcm[i] <= -32760;
    return count;
}

int probe_record(const probe_options_t *options)
{
    FILE *play_file = NULL, *mic_file = NULL, *ref_file = NULL, *out_file = NULL, *csv = NULL;
    snd_pcm_t *capture = NULL, *playback = NULL;
    snd_pcm_uframes_t capture_buffer, playback_buffer;
    probe_dsp_t dsp = {0};
    struct sigaction action = {0}, old_int, old_term;
    int signals_set = 0, linked = 0, rc = 1;
    int64_t input_bytes;
    int16_t *prime = NULL;
    play_file = fopen(options->play, "rb");
    if (!play_file) { perror(options->play); goto done; }
    if (probe_pcm_size(play_file, &input_bytes)) goto done;
    if ((int64_t)options->seconds * PROBE_RATE < input_bytes / 2 + PROBE_RATE) {
        fprintf(stderr, "--seconds must cover the playback file plus at least 1 second of echo tail\n");
        goto done;
    }
    if (probe_dsp_init(&dsp, options)) {
        fprintf(stderr, "AEC initialization failed\n");
        goto done;
    }
    /* A busy device is an error. This tool never kills another audio application. */
    if (setup_pcm(&capture, options->device, SND_PCM_STREAM_CAPTURE, &capture_buffer) ||
        setup_pcm(&playback, options->device, SND_PCM_STREAM_PLAYBACK, &playback_buffer)) goto done;
    mic_file = output_file(options->prefix, ".mic.raw");
    if (!mic_file) goto done;
    ref_file = output_file(options->prefix, ".ref.raw");
    if (!ref_file) goto done;
    out_file = output_file(options->prefix, ".out.raw");
    if (!out_file) goto done;
    csv = output_file(options->prefix, ".csv");
    if (!csv) goto done;
    if (fprintf(csv, "frame,capture_delay_frames,playback_delay_frames,aec_us,mic_rms,ref_rms,out_rms,mic_clip,ref_clip\n") < 0) goto done;

    stopped = 0;
    action.sa_handler = stop_recording;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &old_int)) { perror("SIGINT"); goto done; }
    if (sigaction(SIGTERM, &action, &old_term)) {
        perror("SIGTERM");
        sigaction(SIGINT, &old_int, NULL);
        goto done;
    }
    signals_set = 1;
    prime = calloc(playback_buffer, sizeof(int16_t));
    if (!prime || transfer(playback, prime, playback_buffer, 1)) goto done;
    free(prime); prime = NULL;

    int error = snd_pcm_link(capture, playback);
    if (error == 0) linked = 1;
    else fprintf(stderr, "linked start unavailable: %s; using sequential start (alignment must be measured)\n",
                 snd_strerror(error));
    if ((!linked && (error = snd_pcm_start(playback)) < 0) ||
        (error = snd_pcm_start(capture)) < 0) {
        fprintf(stderr, "ALSA start failed: %s\n", snd_strerror(error));
        goto done;
    }
    fprintf(stderr, "recording %d seconds; mic stays open; gain=%d%% linked_start=%d denoise=%d\n",
            options->seconds, options->gain_pct, linked, options->denoise);

    int16_t mic[PROBE_FRAME], ref[PROBE_FRAME], out[PROBE_FRAME];
    for (int frame = 0; frame < options->seconds * 100 && !stopped; frame++) {
        if (transfer(capture, mic, PROBE_FRAME, 0)) goto done;
        snd_pcm_sframes_t capture_delay, playback_delay;
        error = snd_pcm_delay(capture, &capture_delay);
        if (error < 0) { fprintf(stderr, "capture delay failed: %s\n", snd_strerror(error)); goto done; }
        size_t got = fread(ref, sizeof(int16_t), PROBE_FRAME, play_file);
        if (ferror(play_file)) { perror("playback file read"); goto done; }
        for (size_t i = 0; i < got; i++) ref[i] = (int16_t)((int32_t)ref[i] * options->gain_pct / 100);
        memset(ref + got, 0, (PROBE_FRAME - got) * sizeof(int16_t));
        if (transfer(playback, ref, PROBE_FRAME, 1)) goto done;
        error = snd_pcm_delay(playback, &playback_delay);
        if (error < 0) { fprintf(stderr, "playback delay failed: %s\n", snd_strerror(error)); goto done; }
        /* Reference is the submitted DAC signal. It is NOT presumed sample-aligned
         * with mic; the adaptive tail covers delay, and dumps permit its measurement. */
        double elapsed = probe_process(&dsp, mic, ref, out);
        if (fwrite(mic, sizeof(int16_t), PROBE_FRAME, mic_file) != PROBE_FRAME ||
            fwrite(ref, sizeof(int16_t), PROBE_FRAME, ref_file) != PROBE_FRAME ||
            fwrite(out, sizeof(int16_t), PROBE_FRAME, out_file) != PROBE_FRAME ||
            fprintf(csv, "%d,%ld,%ld,%.1f,%.1f,%.1f,%.1f,%d,%d\n", frame,
                    (long)capture_delay, (long)playback_delay, elapsed,
                    probe_rms(mic, PROBE_FRAME), probe_rms(ref, PROBE_FRAME),
                    probe_rms(out, PROBE_FRAME), clipping(mic), clipping(ref)) < 0) {
            fprintf(stderr, "recording write failed (check free storage)\n");
            goto done;
        }
    }
    rc = stopped ? 130 : 0;
done:
    /* Drop, rather than drain, also stops queued audio promptly on Ctrl-C/error. */
    if (linked) snd_pcm_unlink(capture);
    if (capture) { snd_pcm_drop(capture); snd_pcm_close(capture); }
    if (playback) { snd_pcm_drop(playback); snd_pcm_close(playback); }
    if (signals_set) {
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTERM, &old_term, NULL);
    }
    if (stopped && rc) rc = 130;
    free(prime);
    FILE *files[] = {play_file, mic_file, ref_file, out_file, csv};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
        if (files[i] && fclose(files[i])) { perror("recording close"); rc = 1; }
    probe_dsp_close(&dsp);
    fprintf(stderr, "record status=%s rc=%d; exit 0 only proves uninterrupted I/O, not acoustic quality\n",
            rc ? "incomplete" : "complete", rc);
    return rc;
}
