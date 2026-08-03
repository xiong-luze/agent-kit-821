/* ringbuf.c —— 见 ringbuf.h */
#include "ringbuf.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static int16_t        *g_buf = NULL;
static size_t          g_cap = 0;
static size_t          g_head = 0, g_tail = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

void ringbuf_init(int sample_rate, int seconds)
{
    free(g_buf);
    g_cap = (size_t)sample_rate * (size_t)seconds;
    g_buf = (int16_t *)malloc(g_cap * sizeof(int16_t));
    g_head = g_tail = 0;
}

size_t ringbuf_avail(void)
{
    pthread_mutex_lock(&g_mtx);
    size_t n = (g_tail + g_cap - g_head) % g_cap;
    pthread_mutex_unlock(&g_mtx);
    return n;
}

void ringbuf_push(const int16_t *pcm, size_t n)
{
    if (!g_buf) return;
    pthread_mutex_lock(&g_mtx);
    for (size_t i = 0; i < n; i++) {
        size_t next = (g_tail + 1) % g_cap;
        if (next == g_head) break;          /* 满了就丢，别覆盖未播的 */
        g_buf[g_tail] = pcm[i];
        g_tail = next;
    }
    pthread_mutex_unlock(&g_mtx);
}

size_t ringbuf_pop_frame(int16_t *out, size_t frame_samples)
{
    if (!g_buf) { memset(out, 0, frame_samples * sizeof(int16_t)); return 0; }
    pthread_mutex_lock(&g_mtx);
    size_t got = 0;
    while (got < frame_samples && g_head != g_tail) {
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
