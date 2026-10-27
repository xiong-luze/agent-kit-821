/* ringbuf.c —— 见 ringbuf.h */
#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static int16_t        *g_buf = NULL;
static size_t          g_cap = 0;
static size_t          g_head = 0, g_tail = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

int ringbuf_init(int sample_rate, int seconds)
{
    if (sample_rate <= 0 || seconds <= 0 || (size_t)seconds > SIZE_MAX / (size_t)sample_rate /
        sizeof(int16_t)) return -1;
    size_t capacity = (size_t)sample_rate * (size_t)seconds;
    if (capacity < 2) return -1;
    int16_t *next = malloc(capacity * sizeof(int16_t));
    if (!next) return -1;
    pthread_mutex_lock(&g_mtx);
    free(g_buf);
    g_buf = next;
    g_cap = capacity;
    g_head = g_tail = 0;
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

void ringbuf_free(void)
{
    pthread_mutex_lock(&g_mtx);
    free(g_buf);
    g_buf = NULL;
    g_cap = g_head = g_tail = 0;
    pthread_mutex_unlock(&g_mtx);
}

size_t ringbuf_avail(void)
{
    pthread_mutex_lock(&g_mtx);
    size_t n = g_cap ? (g_tail + g_cap - g_head) % g_cap : 0;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

size_t ringbuf_push(const int16_t *pcm, size_t n)
{
    if (!pcm) return 0;
    pthread_mutex_lock(&g_mtx);
    size_t accepted = 0;
    for (; g_cap && accepted < n; accepted++) {
        size_t next = (g_tail + 1) % g_cap;
        if (next == g_head) break;          /* 满了就丢，别覆盖未播的 */
        g_buf[g_tail] = pcm[accepted];
        g_tail = next;
    }
    pthread_mutex_unlock(&g_mtx);
    return accepted;
}

size_t ringbuf_pop_frame(int16_t *out, size_t frame_samples)
{
    if (!out) return 0;
    pthread_mutex_lock(&g_mtx);
    size_t got = 0;
    while (g_cap && got < frame_samples && g_head != g_tail) {
        out[got++] = g_buf[g_head];
        g_head = (g_head + 1) % g_cap;
    }
    pthread_mutex_unlock(&g_mtx);
    for (size_t i = got; i < frame_samples; i++) out[i] = 0;
    return got;
}

void ringbuf_clear(void)
{
    pthread_mutex_lock(&g_mtx);
    g_head = g_tail = 0;
    pthread_mutex_unlock(&g_mtx);
}
