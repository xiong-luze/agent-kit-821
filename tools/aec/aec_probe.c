#include "probe.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double monotonic_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000.0 + t.tv_nsec / 1000.0;
}

int probe_dsp_init(probe_dsp_t *dsp, const probe_options_t *options)
{
    memset(dsp, 0, sizeof(*dsp));
    dsp->echo = speex_echo_state_init(PROBE_FRAME, PROBE_RATE * options->tail_ms / 1000);
    if (!dsp->echo) return -1;
    int rate = PROBE_RATE;
    if (speex_echo_ctl(dsp->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate) != 0) return -1;
    if (options->denoise) {
        dsp->pre = speex_preprocess_state_init(PROBE_FRAME, PROBE_RATE);
        if (!dsp->pre) return -1;
        int on = 1, off = 0;
        if (speex_preprocess_ctl(dsp->pre, SPEEX_PREPROCESS_SET_DENOISE, &on) != 0 ||
            speex_preprocess_ctl(dsp->pre, SPEEX_PREPROCESS_SET_AGC, &off) != 0 ||
            speex_preprocess_ctl(dsp->pre, SPEEX_PREPROCESS_SET_ECHO_STATE, dsp->echo) != 0)
            return -1;
    }
    return 0;
}

double probe_process(probe_dsp_t *dsp, const int16_t *mic, const int16_t *ref,
                     int16_t *out)
{
    double start = monotonic_us();
    speex_echo_cancellation(dsp->echo, mic, ref, out);
    if (dsp->pre) speex_preprocess_run(dsp->pre, out);
    double elapsed = monotonic_us() - start;
    dsp->frames++;
    dsp->total_us += elapsed;
    if (elapsed > dsp->max_us) dsp->max_us = elapsed;
    return elapsed;
}

void probe_dsp_close(probe_dsp_t *dsp)
{
    if (dsp->pre) speex_preprocess_state_destroy(dsp->pre);
    if (dsp->echo) speex_echo_state_destroy(dsp->echo);
    if (dsp->frames)
        fprintf(stderr, "AEC frames=%llu mean_us=%.1f max_us=%.1f (10 ms budget/frame)\n",
                (unsigned long long)dsp->frames, dsp->total_us / dsp->frames, dsp->max_us);
    memset(dsp, 0, sizeof(*dsp));
}

FILE *probe_new_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror(path); return NULL; }
    FILE *file = fdopen(fd, "wb");
    if (!file) { perror(path); close(fd); }
    return file;
}

int probe_pcm_size(FILE *file, int64_t *bytes)
{
    struct stat st;
    if (fstat(fileno(file), &st) != 0) { perror("input stat"); return -1; }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size % 2) {
        fprintf(stderr, "input must be a nonempty regular file of 16-bit PCM samples\n");
        return -1;
    }
    *bytes = st.st_size;
    return 0;
}

double probe_rms(const int16_t *pcm, int count)
{
    double energy = 0;
    for (int i = 0; i < count; i++) energy += (double)pcm[i] * pcm[i];
    return sqrt(energy / count);
}

static int replay(const probe_options_t *options)
{
    FILE *mic_file = NULL, *ref_file = NULL, *out_file = NULL;
    probe_dsp_t dsp = {0};
    int rc = 1;
    int64_t mic_bytes, ref_bytes;
    mic_file = fopen(options->mic, "rb");
    if (!mic_file) { perror(options->mic); goto done; }
    ref_file = fopen(options->ref, "rb");
    if (!ref_file) { perror(options->ref); goto done; }
    if (probe_pcm_size(mic_file, &mic_bytes) || probe_pcm_size(ref_file, &ref_bytes)) goto done;
    if (mic_bytes != ref_bytes) {
        fprintf(stderr, "mic and ref must have the same length\n");
        goto done;
    }
    if (probe_dsp_init(&dsp, options)) {
        fprintf(stderr, "AEC initialization failed\n");
        goto done;
    }
    out_file = probe_new_file(options->out);
    if (!out_file) goto done;

    int16_t mic[PROBE_FRAME], ref[PROBE_FRAME], out[PROBE_FRAME];
    for (int64_t remaining = mic_bytes / 2; remaining > 0; remaining -= PROBE_FRAME) {
        size_t count = remaining < PROBE_FRAME ? (size_t)remaining : PROBE_FRAME;
        memset(mic, 0, sizeof(mic));
        memset(ref, 0, sizeof(ref));
        if (fread(mic, sizeof(int16_t), count, mic_file) != count ||
            fread(ref, sizeof(int16_t), count, ref_file) != count) {
            fprintf(stderr, "PCM input read failed\n");
            goto done;
        }
        probe_process(&dsp, mic, ref, out);
        if (fwrite(out, sizeof(int16_t), count, out_file) != count) {
            perror("PCM output write");
            goto done;
        }
    }
    fprintf(stderr, "replay samples=%lld denoise=%d tail_ms=%d\n",
            (long long)(mic_bytes / 2), options->denoise, options->tail_ms);
    rc = 0;
done:
    if (mic_file && fclose(mic_file)) rc = 1;
    if (ref_file && fclose(ref_file)) rc = 1;
    if (out_file && fclose(out_file)) { perror("PCM output close"); rc = 1; }
    probe_dsp_close(&dsp);
    return rc;
}

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage (all PCM: raw S16_LE, 16000 Hz, mono):\n"
        "  %s replay --mic mic.raw --ref ref.raw --out out.raw [--tail-ms 200] [--denoise]\n"
        "  %s record --play speech.raw --prefix /path/new-run [--device hw:0,0]\n"
        "      [--seconds 20] [--gain-pct 25] [--tail-ms 200] [--denoise]\n"
        "Output files are created exclusively; existing recordings are never overwritten.\n"
        "No cloud, VAD, or automatic barge-in is enabled by this experiment.\n"
#ifdef HAVE_ALSA
        "ALSA record backend: enabled\n",
#else
        "ALSA record backend: disabled (replay only)\n",
#endif
        program, program);
}

static int number(const char *value, int min, int max, int *out)
{
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end || n < min || n > max) return -1;
    *out = (int)n;
    return 0;
}

int main(int argc, char **argv)
{
    probe_options_t options = {.device = "hw:0,0", .tail_ms = 200, .seconds = 20, .gain_pct = 25};
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian != 1) {
        fprintf(stderr, "this raw PCM probe requires a little-endian host\n");
        return 1;
    }
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    if (argc < 2 || (strcmp(argv[1], "replay") && strcmp(argv[1], "record"))) goto bad;
    int recording = !strcmp(argv[1], "record");
    for (int i = 2; i < argc; i++) {
        const char *key = argv[i];
        if (!strcmp(key, "--denoise")) { options.denoise = 1; continue; }
        if (i + 1 >= argc) goto bad;
        const char *value = argv[++i];
        if (!strcmp(key, "--tail-ms")) {
            if (number(value, 20, 1000, &options.tail_ms)) goto bad;
        } else if (!recording && !strcmp(key, "--mic")) options.mic = value;
        else if (!recording && !strcmp(key, "--ref")) options.ref = value;
        else if (!recording && !strcmp(key, "--out")) options.out = value;
        else if (recording && !strcmp(key, "--play")) options.play = value;
        else if (recording && !strcmp(key, "--prefix")) options.prefix = value;
        else if (recording && !strcmp(key, "--device")) options.device = value;
        else if (recording && !strcmp(key, "--seconds")) {
            if (number(value, 1, 600, &options.seconds)) goto bad;
        } else if (recording && !strcmp(key, "--gain-pct")) {
            if (number(value, 0, 100, &options.gain_pct)) goto bad;
        } else goto bad;
    }
    if (!recording) {
        if (!options.mic || !options.ref || !options.out) goto bad;
        return replay(&options);
    }
    if (!options.play || !options.prefix) goto bad;
#ifdef HAVE_ALSA
    return probe_record(&options);
#else
    fprintf(stderr, "record requires a Linux build with AEC_WITH_ALSA=ON\n");
    return 1;
#endif
bad:
    usage(argv[0]);
    return 2;
}
