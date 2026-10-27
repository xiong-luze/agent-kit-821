/* Bounded acoustic evidence for barge-in; this does not alter or suppress PCM. */
#ifndef ECHO_GUARD_H
#define ECHO_GUARD_H
#include <stddef.h>
#include <stdint.h>

#define ECHO_GUARD_REFERENCE 1024  /* 512ms at the decimated 2kHz rate */
#define ECHO_GUARD_WINDOW 160      /* 80ms evidence window */
#define ECHO_GUARD_DELAY 600       /* search reference leads up to 300ms */

typedef struct {
    int16_t reference[ECHO_GUARD_REFERENCE];
    int16_t residual[ECHO_GUARD_WINDOW];
    unsigned reference_next, residual_next;
} echo_guard_t;

typedef struct {
    int near_allowed;     /* Residual is not strongly explained by past reference. */
    int reference_active;
    int delay_ms;         /* Correlation lag includes DSP latency; not hardware delay. */
    float correlation;
} echo_guard_result_t;

void echo_guard_init(echo_guard_t *guard);
/* Serial audio-thread ownership. PCM is mono16kHz; samples must be a multiple of
 * eight in1..320. Feed every frame, including silence. Returns0 or-1 for bad args.
 * Do not clear on each AEC reset/flush: retained old reference explains room tail.
 * near_allowed is evidence for the reply-state barge detector, not a replacement
 * for near-end VAD, KWS, or acoustic acceptance. Never use it to zero uploaded PCM. */
int echo_guard_process(echo_guard_t *guard, const int16_t *reference,
                       const int16_t *residual, size_t samples,
                       echo_guard_result_t *result);
#endif
