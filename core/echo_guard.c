#include "echo_guard.h"
#include <math.h>
#include <string.h>

/* Fixed80ms window,2kHz boxcar-decimated samples, all0.5ms causal lag points.
 * At most601*160 integer MACs per call (~4.8M/s for20ms calls with active reference).
 * Strong reference correlation means loud residual alone is not near-speech
 * evidence, even while the adaptive AEC is still converging. */
#define REFERENCE_MASK (ECHO_GUARD_REFERENCE - 1)
#define CORRELATION_SQUARED 0.36f
#define REFERENCE_VARIANCE_FLOOR (ECHO_GUARD_WINDOW * 32 * 32)

void echo_guard_init(echo_guard_t *guard)
{
    if (guard) memset(guard, 0, sizeof(*guard));
}

static float score(const echo_guard_t *g, const int16_t *y, int64_t sy,
                   int64_t ey, unsigned lag, int *reference_active)
{
    int64_t sx = 0, xx = 0, xy = 0;
    unsigned begin = (g->reference_next + ECHO_GUARD_REFERENCE -
                      ECHO_GUARD_WINDOW - lag) & REFERENCE_MASK;
    for (unsigned i = 0; i < ECHO_GUARD_WINDOW; i++) {
        int32_t x = g->reference[(begin + i) & REFERENCE_MASK];
        sx += x;
        xx += (int64_t)x * x;
        xy += (int64_t)x * y[i];
    }
    int64_t ex = xx - sx * sx / ECHO_GUARD_WINDOW;
    if (ex < REFERENCE_VARIANCE_FLOOR) return 0;
    *reference_active = 1;
    if (ey <= 0) return 0;
    int64_t cross = xy - sx * sy / ECHO_GUARD_WINDOW;
    float numerator = (float)cross * (float)cross;
    float denominator = (float)ex * (float)ey;
    float result = numerator / denominator;
    return result > 1 ? 1 : result;
}

int echo_guard_process(echo_guard_t *g, const int16_t *reference,
                       const int16_t *residual, size_t samples,
                       echo_guard_result_t *result)
{
    if (!g || !reference || !residual || !result || !samples || samples > 320 || samples % 8)
        return -1;
    for (size_t i = 0; i < samples; i += 8) {
        int32_t x = 0, y = 0;
        for (size_t j = 0; j < 8; j++) { x += reference[i + j]; y += residual[i + j]; }
        g->reference[g->reference_next] = (int16_t)(x / 8);
        g->reference_next = (g->reference_next + 1) & REFERENCE_MASK;
        g->residual[g->residual_next] = (int16_t)(y / 8);
        if (++g->residual_next == ECHO_GUARD_WINDOW) g->residual_next = 0;
    }
    int16_t y[ECHO_GUARD_WINDOW];
    int64_t sy = 0, yy = 0;
    for (unsigned i = 0; i < ECHO_GUARD_WINDOW; i++) {
        y[i] = g->residual[(g->residual_next + i) % ECHO_GUARD_WINDOW];
        sy += y[i];
        yy += (int64_t)y[i] * y[i];
    }
    int64_t ey = yy - sy * sy / ECHO_GUARD_WINDOW;
    /* Every searched window lies within these760 history samples. Total
     * centered energy bounds the centered energy of any subwindow, so this
     * silent-reference fast path cannot hide an energetic candidate window. */
    int64_t ref_sum = 0, ref_squared = 0;
    unsigned recent = ECHO_GUARD_WINDOW + ECHO_GUARD_DELAY;
    unsigned begin = (g->reference_next + ECHO_GUARD_REFERENCE - recent) & REFERENCE_MASK;
    for (unsigned i = 0; i < recent; i++) {
        int32_t value = g->reference[(begin + i) & REFERENCE_MASK];
        ref_sum += value;
        ref_squared += (int64_t)value * value;
    }
    int64_t reference_energy = ref_squared - ref_sum * ref_sum / recent;
    if (reference_energy < REFERENCE_VARIANCE_FLOOR || ey <= 0) {
        result->reference_active = reference_energy >= REFERENCE_VARIANCE_FLOOR;
        result->near_allowed = 1;
        result->correlation = 0;
        result->delay_ms = 0;
        return 0;
    }
    float best = 0;
    unsigned best_lag = 0;
    int active = 0;
    for (unsigned lag = 0; lag <= ECHO_GUARD_DELAY; lag++) {
        float value = score(g, y, sy, ey, lag, &active);
        if (value > best) { best = value; best_lag = lag; }
    }
    result->reference_active = active;
    result->correlation = sqrtf(best);
    result->delay_ms = (int)(best_lag / 2);
    result->near_allowed = !active || best < CORRELATION_SQUARED;
    return 0;
}
