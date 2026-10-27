/* A KWS summary can join repeated detections. It is display text, not one token. */
#include "core/wake_word.h"
#include <pocketsphinx.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

const char *joined_hypothesis(ps_decoder_t *decoder, int32 *score)
{
    const char *hypothesis = ps_get_hyp(decoder, score);
    return hypothesis ? "你好涂鸦 你好涂鸦" : NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    wake_word_config_t config;
    wake_word_config_defaults(&config);
    config.model_dir = argv[1]; config.dictionary = argv[2];
    FILE *file = fopen(argv[3], "rb"); assert(file);
    assert(wake_word_start(&config) == 0);
    int16_t frame[320]; size_t count; int hits = 0;
    struct timespec period = {0, 20000000};
    while ((count = fread(frame, sizeof(*frame), 320, file)) > 0) {
        assert(wake_word_feed(frame, count) == 0);
        nanosleep(&period, NULL);
        int hit = wake_word_poll(); assert(hit >= 0); hits += hit;
    }
    for (int i = 0; i < 10; i++) {
        nanosleep(&period, NULL);
        int hit = wake_word_poll(); assert(hit >= 0); hits += hit;
    }
    wake_word_stop(); fclose(file);
    assert(hits == 1); /* A joined rendering must still produce one latched match. */
    puts("joined keyword result accepted without relying on rendered summary equality");
    return 0;
}
