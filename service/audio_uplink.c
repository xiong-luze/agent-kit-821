#include "audio_uplink.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct audio_uplink {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_t worker;
    audio_uplink_item_t *items;
    size_t capacity, head, count;
    int stopping, error;
    int trace, active;
    audio_uplink_kind_t active_kind;
    uint64_t active_generation;
    int64_t active_since_ms;
    audio_uplink_send_t send;
    void *context;
};

static int64_t clock_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void *worker_main(void *context)
{
    audio_uplink_t *q = context;
    pthread_mutex_lock(&q->mutex);
    while (!q->stopping && !q->error) {
        while (!q->count && !q->stopping && !q->error)
            pthread_cond_wait(&q->ready, &q->mutex);
        if (q->stopping || q->error) break;
        audio_uplink_item_t item = q->items[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        q->active = 1;
        q->active_kind = item.kind;
        q->active_generation = item.generation;
        q->active_since_ms = q->trace ? clock_ms() : 0;
        pthread_mutex_unlock(&q->mutex);
        int rc = q->send(q->context, &item);
        pthread_mutex_lock(&q->mutex);
        q->active = 0;
        if (rc && !q->error) q->error = rc < 0 ? rc : -EIO;
    }
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    return NULL;
}

audio_uplink_t *audio_uplink_create(size_t capacity, audio_uplink_send_t send, void *context)
{
    if (capacity < 2 || capacity > 256 || !send) return NULL;
    audio_uplink_t *q = calloc(1, sizeof(*q));
    if (!q) return NULL;
    q->items = calloc(capacity, sizeof(*q->items));
    if (!q->items) { free(q); return NULL; }
    q->capacity = capacity;
    q->send = send;
    q->context = context;
    const char *trace = getenv("V821_VOICE_TRACE");
    q->trace = trace && !strcmp(trace, "1");
    if (pthread_mutex_init(&q->mutex, NULL)) goto fail;
    if (pthread_cond_init(&q->ready, NULL)) {
        pthread_mutex_destroy(&q->mutex);
        goto fail;
    }
    if (pthread_create(&q->worker, NULL, worker_main, q)) {
        pthread_cond_destroy(&q->ready);
        pthread_mutex_destroy(&q->mutex);
        goto fail;
    }
    return q;
fail:
    free(q->items);
    free(q);
    return NULL;
}

int audio_uplink_post(audio_uplink_t *q, audio_uplink_kind_t kind,
                      const int16_t *pcm, size_t samples, uint64_t generation)
{
    if (!q || kind < AUDIO_UPLINK_START || kind > AUDIO_UPLINK_INTERRUPT ||
        samples > AUDIO_UPLINK_MAX_SAMPLES ||
        (kind == AUDIO_UPLINK_CHUNK ? (!pcm || !samples) : samples != 0)) return -EINVAL;
    pthread_mutex_lock(&q->mutex);
    int rc = q->error ? q->error : q->stopping ? -EPIPE : 0;
    if (!rc && q->count == q->capacity) {
        if (q->trace)
            fprintf(stderr, "[发送队列诊断] queued=%zu active=%d kind=%d gen=%llu active_ms=%lld\n",
                    q->count, q->active, q->active_kind,
                    (unsigned long long)q->active_generation,
                    (long long)(q->active ? clock_ms() - q->active_since_ms : 0));
        q->error = rc = -ENOBUFS;
        q->count = 0;
        pthread_cond_signal(&q->ready);
    }
    if (!rc) {
        audio_uplink_item_t *item = &q->items[(q->head + q->count) % q->capacity];
        item->kind = kind;
        item->generation = generation;
        item->samples = samples;
        if (samples) memcpy(item->pcm, pcm, samples * sizeof(*pcm));
        q->count++;
        pthread_cond_signal(&q->ready);
    }
    pthread_mutex_unlock(&q->mutex);
    return rc;
}

int audio_uplink_interrupt(audio_uplink_t *q, uint64_t generation)
{
    if (!q) return -EINVAL;
    pthread_mutex_lock(&q->mutex);
    int rc = q->error ? q->error : q->stopping ? -EPIPE : 0;
    if (!rc) {
        q->head = 0;
        q->count = 1;
        q->items[0].kind = AUDIO_UPLINK_INTERRUPT;
        q->items[0].generation = generation;
        q->items[0].samples = 0;
        pthread_cond_signal(&q->ready);
    }
    pthread_mutex_unlock(&q->mutex);
    return rc;
}

int audio_uplink_error(audio_uplink_t *q)
{
    if (!q) return -EINVAL;
    pthread_mutex_lock(&q->mutex);
    int error = q->error;
    pthread_mutex_unlock(&q->mutex);
    return error;
}

void audio_uplink_destroy(audio_uplink_t *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    q->stopping = 1;
    q->count = 0;
    pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->mutex);
    pthread_join(q->worker, NULL);
    pthread_cond_destroy(&q->ready);
    pthread_mutex_destroy(&q->mutex);
    free(q->items);
    free(q);
}
