/* Standalone audio experiment; the product half-duplex path does not use this. */
#ifndef AEC_PROBE_H
#define AEC_PROBE_H

#include <stdint.h>
#include <stdio.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>

#define PROBE_RATE 16000
#define PROBE_FRAME 160                 /* 10 ms */

typedef struct {
    const char *mic, *ref, *out;        /* replay */
    const char *device, *play, *prefix; /* record */
    int tail_ms, denoise, seconds, gain_pct;
} probe_options_t;

typedef struct {
    SpeexEchoState *echo;
    SpeexPreprocessState *pre;
    uint64_t frames;
    double total_us, max_us;
} probe_dsp_t;

int probe_dsp_init(probe_dsp_t *dsp, const probe_options_t *options);
double probe_process(probe_dsp_t *dsp, const int16_t *mic, const int16_t *ref,
                     int16_t *out);
void probe_dsp_close(probe_dsp_t *dsp);
FILE *probe_new_file(const char *path);
int probe_pcm_size(FILE *file, int64_t *bytes);
double probe_rms(const int16_t *pcm, int count);
int probe_record(const probe_options_t *options);

#endif
