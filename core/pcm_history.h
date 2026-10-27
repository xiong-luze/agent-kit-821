#ifndef PCM_HISTORY_H
#define PCM_HISTORY_H
#include <stddef.h>
#include <stdint.h>
typedef struct { int16_t *data; size_t capacity, count, next; } pcm_history_t;
/* Audio-thread owned. Retains the most recent capacity PCM samples. */
int pcm_history_init(pcm_history_t *history, size_t capacity);
void pcm_history_push(pcm_history_t *history, const int16_t *pcm, size_t count);
size_t pcm_history_read(const pcm_history_t *history, int16_t *pcm, size_t capacity);
void pcm_history_clear(pcm_history_t *history);
void pcm_history_free(pcm_history_t *history);
#endif
