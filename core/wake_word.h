/* Local PCM keyword spotting. Feed/poll never run inference on the audio thread. */
#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *model_dir;
    const char *dictionary;
    const char *phrase;
    double threshold;
    int activity_min_rms; /* Raw quiet/voice boundary for preserving channel baseline;0 disables quiet protection. */
} wake_word_config_t;

void wake_word_config_defaults(wake_word_config_t *config);
/* External model and tiny dictionary must match the configured UTF-8 phrase.
 * Start/stop are owner-thread lifecycle operations; start enables detection. */
int wake_word_start(const wake_word_config_t *config);
/* Accept 1..320 mono PCM samples at 16kHz. Returns 0 accepted/disabled,
 * -1 invalid input, unavailable worker, or terminal inference/queue error. */
int wake_word_feed(const int16_t *pcm, size_t count);
/* 1 consumes a latched hit, 0 no hit, -1 unavailable/terminal error. */
int wake_word_poll(void);
/* An actual enabled-state change clears queued audio and hits, resets the decoder,
 * and invalidates an in-flight result. Repeated calls with the same value are idempotent. */
int wake_word_set_enabled(int enabled);
void wake_word_stop(void);

#endif
