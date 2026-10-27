#include "pcm_history.h"
#include <stdlib.h>
#include <string.h>

int pcm_history_init(pcm_history_t *h, size_t capacity)
{
    if (!h || !capacity || capacity > SIZE_MAX / sizeof(int16_t)) return -1;
    memset(h, 0, sizeof(*h));
    h->data = calloc(capacity, sizeof(int16_t));
    if (!h->data) return -1;
    h->capacity = capacity;
    return 0;
}
void pcm_history_push(pcm_history_t *h, const int16_t *pcm, size_t count)
{
    if (!h || !h->capacity || !pcm) return;
    for (size_t i = 0; i < count; i++) {
        h->data[h->next] = pcm[i];
        h->next = (h->next + 1) % h->capacity;
        if (h->count < h->capacity) h->count++;
    }
}
size_t pcm_history_read(const pcm_history_t *h, int16_t *pcm, size_t capacity)
{
    if (!h || !h->capacity || !pcm) return 0;
    size_t n = h->count < capacity ? h->count : capacity;
    size_t begin = (h->next + h->capacity - n) % h->capacity;
    for (size_t i = 0; i < n; i++) pcm[i] = h->data[(begin + i) % h->capacity];
    return n;
}
void pcm_history_clear(pcm_history_t *h) { if (h) h->count = h->next = 0; }
void pcm_history_free(pcm_history_t *h)
{
    if (!h) return;
    free(h->data);
    memset(h, 0, sizeof(*h));
}
