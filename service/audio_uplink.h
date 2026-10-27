/* Bounded PCM/control queue. Only its worker invokes the blocking transport. */
#ifndef AUDIO_UPLINK_H
#define AUDIO_UPLINK_H
#include <stddef.h>
#include <stdint.h>

#define AUDIO_UPLINK_MAX_SAMPLES 320
typedef enum {
    AUDIO_UPLINK_START, AUDIO_UPLINK_CHUNK, AUDIO_UPLINK_END, AUDIO_UPLINK_INTERRUPT
} audio_uplink_kind_t;
typedef struct {
    audio_uplink_kind_t kind;
    uint64_t generation;
    size_t samples;
    int16_t pcm[AUDIO_UPLINK_MAX_SAMPLES];
} audio_uplink_item_t;
typedef struct audio_uplink audio_uplink_t;
typedef int (*audio_uplink_send_t)(void *context, const audio_uplink_item_t *item);

audio_uplink_t *audio_uplink_create(size_t capacity, audio_uplink_send_t send, void *context);
int audio_uplink_post(audio_uplink_t *queue, audio_uplink_kind_t kind,
                      const int16_t *pcm, size_t samples, uint64_t generation);
/* Discards queued work, queues BREAK first. An already executing send cannot be
 * cancelled; local playback must already be stopped before this call. */
int audio_uplink_interrupt(audio_uplink_t *queue, uint64_t generation);
int audio_uplink_error(audio_uplink_t *queue);
/* Discards queued work and joins the worker. Shutdown may wait for the one
 * in-flight transport call, never call this from that transport callback. */
void audio_uplink_destroy(audio_uplink_t *queue);
#endif
