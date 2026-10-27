#define _POSIX_C_SOURCE 200809L
#include "audio_uplink.h"
#include "turn_guard.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int blocked, entered, count, fail_chunk;
    audio_uplink_item_t seen[32];
} transport_t;

static int send_item(void *context, const audio_uplink_item_t *item)
{
    transport_t *t = context;
    pthread_mutex_lock(&t->mutex);
    t->entered = 1;
    pthread_cond_broadcast(&t->cond);
    while (t->blocked) pthread_cond_wait(&t->cond, &t->mutex);
    assert(t->count < 32);
    t->seen[t->count++] = *item;
    pthread_cond_broadcast(&t->cond);
    int error = t->fail_chunk && item->kind == AUDIO_UPLINK_CHUNK ? -EIO : 0;
    pthread_mutex_unlock(&t->mutex);
    return error;
}

static void init_transport(transport_t *t)
{
    memset(t, 0, sizeof(*t));
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->cond, NULL);
}

static void wait_for(transport_t *t, int entered, int count)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&t->mutex);
    while ((entered && !t->entered) || t->count < count)
        assert(pthread_cond_timedwait(&t->cond, &t->mutex, &deadline) == 0);
    pthread_mutex_unlock(&t->mutex);
}

static void release(transport_t *t)
{
    pthread_mutex_lock(&t->mutex);
    t->blocked = 0;
    pthread_cond_broadcast(&t->cond);
    pthread_mutex_unlock(&t->mutex);
}

static void destroy_transport(transport_t *t)
{
    pthread_cond_destroy(&t->cond);
    pthread_mutex_destroy(&t->mutex);
}

static void test_interrupt_discards_queued_old_pcm(void)
{
    transport_t t;
    init_transport(&t);
    t.blocked = 1;
    audio_uplink_t *q = audio_uplink_create(8, send_item, &t);
    assert(q);
    assert(audio_uplink_post(q, AUDIO_UPLINK_START, NULL, 0, 1) == 0);
    wait_for(&t, 1, 0);
    int16_t samples[320] = {123};
    assert(audio_uplink_post(q, AUDIO_UPLINK_CHUNK, samples, 320, 1) == 0);
    assert(audio_uplink_post(q, AUDIO_UPLINK_END, NULL, 0, 1) == 0);
    assert(audio_uplink_interrupt(q, 2) == 0);
    assert(audio_uplink_post(q, AUDIO_UPLINK_START, NULL, 0, 2) == 0);
    samples[0] = 456;
    assert(audio_uplink_post(q, AUDIO_UPLINK_CHUNK, samples, 320, 2) == 0);
    samples[0] = 999;  /* The queue must own a copy. */
    assert(audio_uplink_post(q, AUDIO_UPLINK_END, NULL, 0, 2) == 0);
    release(&t);
    wait_for(&t, 0, 5);
    assert(t.seen[0].kind == AUDIO_UPLINK_START && t.seen[0].generation == 1);
    assert(t.seen[1].kind == AUDIO_UPLINK_INTERRUPT);
    assert(t.seen[2].kind == AUDIO_UPLINK_START && t.seen[2].generation == 2);
    assert(t.seen[3].kind == AUDIO_UPLINK_CHUNK && t.seen[3].pcm[0] == 456);
    assert(t.seen[4].kind == AUDIO_UPLINK_END);
    audio_uplink_destroy(q);
    destroy_transport(&t);
}

static void test_overflow_is_explicit_and_stops_pending_work(void)
{
    transport_t t;
    init_transport(&t);
    t.blocked = 1;
    audio_uplink_t *q = audio_uplink_create(2, send_item, &t);
    assert(q);
    assert(audio_uplink_post(q, AUDIO_UPLINK_START, NULL, 0, 1) == 0);
    wait_for(&t, 1, 0);
    int16_t samples[320] = {0};
    assert(audio_uplink_post(q, AUDIO_UPLINK_CHUNK, samples, 320, 1) == 0);
    assert(audio_uplink_post(q, AUDIO_UPLINK_CHUNK, samples, 320, 1) == 0);
    assert(audio_uplink_post(q, AUDIO_UPLINK_END, NULL, 0, 1) == -ENOBUFS);
    assert(audio_uplink_error(q) == -ENOBUFS);
    release(&t);
    audio_uplink_destroy(q);
    assert(t.count == 1);
    destroy_transport(&t);
}

static void test_transport_failure_is_reported(void)
{
    transport_t t;
    init_transport(&t);
    t.fail_chunk = 1;
    audio_uplink_t *q = audio_uplink_create(4, send_item, &t);
    assert(q);
    int16_t samples[1] = {3};
    assert(audio_uplink_post(q, AUDIO_UPLINK_CHUNK, samples, 1, 1) == 0);
    wait_for(&t, 0, 1);
    struct timespec until, now, pause = {0, 1000000};
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += 3;
    while (!audio_uplink_error(q)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        assert(now.tv_sec < until.tv_sec ||
               (now.tv_sec == until.tv_sec && now.tv_nsec < until.tv_nsec));
        nanosleep(&pause, NULL);
    }
    assert(audio_uplink_error(q) == -EIO);
    assert(audio_uplink_post(q, AUDIO_UPLINK_START, NULL, 0, 2) == -EIO);
    audio_uplink_destroy(q);
    assert(t.count == 1);
    destroy_transport(&t);
}

static void test_id_capacity_fails_closed_instead_of_evicting(void)
{
    turn_guard_t guard;
    turn_guard_init(&guard);
    uint64_t generation = turn_guard_begin(&guard);
    turn_guard_request_sent(&guard, generation);
    assert(turn_guard_accept(&guard, "first", TURN_GUARD_ASR));
    char id[32];
    for (unsigned i = 1; i < TURN_GUARD_CURRENT_IDS; i++) {
        snprintf(id, sizeof(id), "continuation-%u", i);
        assert(turn_guard_accept(&guard, id, TURN_GUARD_START));
    }
    assert(!turn_guard_accept(&guard, "overflow", TURN_GUARD_START));
    assert(turn_guard_error(&guard) == -ENOSPC);
    assert(!turn_guard_accept(&guard, "first", TURN_GUARD_MEDIA));
}

static void test_stale_turns_are_not_new_answers(void)
{
    turn_guard_t guard;
    turn_guard_init(&guard);
    uint64_t generation = turn_guard_begin(&guard);
    assert(!turn_guard_accept(&guard, "old", TURN_GUARD_ASR));
    turn_guard_request_sent(&guard, generation);
    assert(!turn_guard_accept(&guard, "", TURN_GUARD_ASR));
    assert(!turn_guard_accept(&guard, "old", TURN_GUARD_MEDIA));
    assert(turn_guard_accept(&guard, "old", TURN_GUARD_ASR));
    assert(turn_guard_accept(&guard, "old", TURN_GUARD_MEDIA));
    assert(turn_guard_accept(&guard, "mcp-1", TURN_GUARD_START));
    assert(turn_guard_accept(&guard, "mcp-1", TURN_GUARD_MEDIA));
    turn_guard_interrupt(&guard);
    assert(!turn_guard_accept(&guard, "old", TURN_GUARD_END));
    assert(!turn_guard_accept(&guard, "mcp-1", TURN_GUARD_MEDIA));
    generation = turn_guard_begin(&guard);
    turn_guard_request_sent(&guard, generation - 1); /* Old START completion. */
    assert(!turn_guard_accept(&guard, "fresh", TURN_GUARD_ASR));
    turn_guard_request_sent(&guard, generation);
    assert(!turn_guard_accept(&guard, "old", TURN_GUARD_ASR));
    assert(!turn_guard_accept(&guard, "fresh", TURN_GUARD_MEDIA));
    assert(turn_guard_accept(&guard, "fresh", TURN_GUARD_ASR));
    assert(!turn_guard_accept(&guard, "old", TURN_GUARD_END));
    assert(!turn_guard_accept(&guard, "unknown", TURN_GUARD_END));
    assert(turn_guard_accept(&guard, "fresh", TURN_GUARD_MEDIA));
    assert(turn_guard_accept(&guard, "fresh", TURN_GUARD_END));
    assert(turn_guard_accept(&guard, "mcp-2", TURN_GUARD_START));
    assert(turn_guard_accept(&guard, "mcp-2", TURN_GUARD_MEDIA));
}

int main(void)
{
    test_interrupt_discards_queued_old_pcm();
    test_overflow_is_explicit_and_stops_pending_work();
    test_transport_failure_is_reported();
    test_stale_turns_are_not_new_answers();
    test_id_capacity_fails_closed_instead_of_evicting();
    puts("session primitives: 5 checks passed");
    return 0;
}
