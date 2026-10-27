#include "core/audio_dsp.h"
#include <aec_lib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int created, freed, initialized, configured, buffered, processed, fail;
static int token;
int32_t WebRtcAec_Create(void **handle) { if (fail == 1) return -1; *handle = &token; created++; return 0; }
int32_t WebRtcAec_Init(void *handle, int32_t rate, int32_t card_rate)
{ assert(handle == &token && rate == 16000 && card_rate == 16000); initialized++; return fail == 2 ? -1 : 0; }
int32_t WebRtcAec_Free(void *handle) { assert(handle == &token); freed++; return 0; }
int WebRtcAec_set_config(void *handle, AecConfig config)
{ assert(handle == &token && config.nlpMode == kAecNlpModerate && !config.skewMode && !config.metricsMode && !config.delay_logging); configured++; return fail == 3 ? -1 : 0; }
int32_t WebRtcAec_BufferFarend(void *handle, const int16_t *ref, int16_t samples)
{ assert(handle == &token && ref && samples == 160); buffered++; return fail == 4 ? -1 : 0; }
int32_t WebRtcAec_Process(void *handle, const int16_t *mic, const int16_t *high,
    int16_t *out, int16_t *out_high, int16_t samples, int16_t delay, int32_t skew)
{ assert(handle == &token && mic && !high && out && !out_high && samples == 160 && delay >= 0 && delay <= 500 && !skew); processed++; memcpy(out,mic,(size_t)samples * sizeof(*out)); return fail == 5 ? -1 : 0; }
int main(void)
{
    assert(dsp_init(16000, 320, 300, 1) == 0);
    assert(created == 1 && initialized == 1 && configured == 1);
    int16_t mic[320]={0}, ref[320]={0}, out[320];
    assert(dsp_set_delay(60) == 0);
    assert(dsp_set_delay(-1) != 0 && dsp_set_delay(501) != 0);
    assert(dsp_process(mic, ref, out) == 0);
    assert(buffered == 2 && processed == 2); /* Both halves must be processed. */
    assert(dsp_reset() == 0);
    assert(initialized == 2 && configured == 2); /* Reset preserves NLP configuration. */
    dsp_deinit(); assert(freed == created && !dsp_enabled());
    for (fail = 1; fail <= 3; fail++) {
        assert(dsp_init(16000,320,300,1) != 0 && !dsp_enabled());
        assert(dsp_process(mic,ref,out) != 0);
        dsp_deinit(); assert(created == freed);
    }
    for (int error = 4; error <= 5; error++) {
        fail = 0; assert(dsp_init(16000,320,300,1) == 0);
        fail = error; memset(out, 1, sizeof(out));
        assert(dsp_process(mic,ref,out) != 0 && !dsp_enabled());
        for (int i = 0; i < 320; i++) assert(out[i] == 0);
        fail = 0; assert(dsp_process(mic,ref,out) != 0); /* No silent bypass after failure. */
        dsp_deinit(); assert(created == freed);
    }
    assert(dsp_init(8000,320,300,1) != 0);
    assert(dsp_init(16000,319,300,1) != 0);
    assert(dsp_init(16000,320,300,1) == 0);
    fail = 3; assert(dsp_reset() != 0 && dsp_process(mic,ref,out) != 0);
    dsp_deinit(); assert(created == freed);
    puts("vendor AEC framing, reset, cleanup and terminal-error checks passed");
    return 0;
}
