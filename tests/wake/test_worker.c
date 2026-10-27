#include "wake_word.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    wake_word_config_t config;
    wake_word_config_defaults(&config);
    assert(strcmp(config.phrase, "你好涂鸦") == 0);
    assert(config.threshold > 0 && config.threshold < 1);
    int16_t silence[320] = {0};
    assert(wake_word_feed(silence, 320) == -1);
    assert(wake_word_poll() == -1);
    assert(wake_word_start(&config) == -1);  /* No implicit English model. */
    wake_word_stop();
    if (argc == 3) {
        config.model_dir = argv[1];
        config.dictionary = argv[2];
        config.phrase = "不存在关键词";
        assert(wake_word_start(&config) == -1);
        config.phrase = "你好涂鸦";
        assert(wake_word_start(&config) == 0);
        assert(wake_word_start(&config) == -1);
        assert(wake_word_set_enabled(0) == 0);
        for (int i = 0; i < 100; i++) assert(wake_word_feed(silence, 320) == 0);
        assert(wake_word_poll() == 0);
        assert(wake_word_set_enabled(1) == 0);
        assert(wake_word_feed(NULL, 10) == -1);
        assert(wake_word_feed(silence, 321) == -1);
        int overflow = 0;
        for (int i = 0; i < 10000; i++) {
            if (wake_word_feed(silence, 320) < 0) { overflow = 1; break; }
        }
        assert(overflow);
        assert(wake_word_poll() == -1);
        wake_word_stop();
        assert(wake_word_start(&config) == 0);
        assert(wake_word_set_enabled(0) == 0);
        assert(wake_word_set_enabled(1) == 0);
        assert(wake_word_poll() == 0);
        wake_word_stop();
    }
    puts("wake worker checks passed");
    return 0;
}
