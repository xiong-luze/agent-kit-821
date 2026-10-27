/* Public API boundary substitute only; no algorithm or vendor implementation. */
#ifndef TEST_AEC_LIB_H
#define TEST_AEC_LIB_H
#include <stdint.h>
enum { kAecNlpConservative, kAecNlpModerate, kAecNlpAggressive };
typedef struct { int16_t nlpMode, skewMode, metricsMode; int delay_logging; } AecConfig;
int32_t WebRtcAec_Create(void **);
int32_t WebRtcAec_Init(void *, int32_t, int32_t);
int32_t WebRtcAec_Free(void *);
int WebRtcAec_set_config(void *, AecConfig);
int32_t WebRtcAec_BufferFarend(void *, const int16_t *, int16_t);
int32_t WebRtcAec_Process(void *, const int16_t *, const int16_t *, int16_t *, int16_t *, int16_t, int16_t, int32_t);
#endif
