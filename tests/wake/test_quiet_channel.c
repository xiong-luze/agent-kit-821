/* Simulate the observable channel mean learned from a long background segment. */
#include "core/wake_word.h"
#include <pocketsphinx.h>
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static char initial_mean[512];
static int frames, restored;
int observe_quiet_channel(ps_decoder_t *decoder, int16 const *pcm, size_t count,
                          int no_search, int full_utt)
{
    pthread_mutex_lock(&mutex);
    if (frames == 0) {
        snprintf(initial_mean, sizeof(initial_mean), "%s", ps_get_cmn(decoder, 0));
        assert(ps_set_cmn(decoder, "0,0,0,0,0,0,0,0,0,0,0,0,0") == 0);
    }
    if (frames >= 60 && !strcmp(ps_get_cmn(decoder, 0), initial_mean)) restored = 1;
    frames++;
    pthread_mutex_unlock(&mutex);
    return ps_process_raw(decoder, pcm, count, no_search, full_utt);
}
int main(int argc, char **argv)
{
    assert(argc == 3);
    wake_word_config_t config; wake_word_config_defaults(&config);
    config.model_dir = argv[1]; config.dictionary = argv[2];
    assert(wake_word_start(&config) == 0);
    int16_t silence[320] = {0}; struct timespec period = {0, 20000000};
    for (int i = 0; i < 65; i++) {
        assert(wake_word_feed(silence, 320) == 0); nanosleep(&period, NULL);
    }
    wake_word_stop();
    pthread_mutex_lock(&mutex);
    assert(frames >= 61 && restored); /* Silence must not preserve a background-only channel mean. */
    pthread_mutex_unlock(&mutex);
    puts("quiet input restores the initial keyword channel mean");
    return 0;
}
