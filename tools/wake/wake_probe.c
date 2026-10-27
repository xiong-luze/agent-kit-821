/* Host/target PCM replay through the same asynchronous KWS worker as the app. */
#include "wake_word.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: %s MODEL_DIR DICTIONARY UTF8_PHRASE PCM16K_MONO_S16LE [THRESHOLD]\n", argv[0]);
        return 1;
    }
    FILE *file = fopen(argv[4], "rb");
    if (!file) { perror(argv[4]); return 1; }
    struct stat info;
    if (fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 || info.st_size % 2) {
        fprintf(stderr, "input must be a nonempty regular file of PCM16k mono S16_LE samples\n");
        fclose(file);
        return 1;
    }
    wake_word_config_t config;
    wake_word_config_defaults(&config);
    config.model_dir = argv[1];
    config.dictionary = argv[2];
    config.phrase = argv[3];
    if (argc == 6) {
        char *end;
        errno = 0;
        config.threshold = strtod(argv[5], &end);
        if (errno || end == argv[5] || *end) {
            fprintf(stderr, "invalid keyword threshold\n");
            fclose(file);
            return 1;
        }
    }
    if (wake_word_start(&config) < 0) { fclose(file); return 1; }
    int16_t pcm[320];
    size_t count;
    int hits = 0, frames = 0, rc = 2;
    struct timespec period = {.tv_sec = 0, .tv_nsec = 20000000};
    while ((count = fread(pcm, sizeof(*pcm), 320, file)) > 0) {
        if (wake_word_feed(pcm, count) < 0) { rc = 1; goto done; }
        nanosleep(&period, NULL);
        int result = wake_word_poll();
        if (result < 0) { rc = 1; goto done; }
        if (result) { hits++; printf("wake frame=%d phrase=%s\n", frames, config.phrase); }
        frames++;
    }
    if (ferror(file)) { perror("PCM read"); rc = 1; goto done; }
    for (int i = 0; i < 10; i++) {
        nanosleep(&period, NULL);
        int result = wake_word_poll();
        if (result < 0) { rc = 1; goto done; }
        hits += result;
    }
    rc = hits ? 0 : 2;
done:
    wake_word_stop();
    fclose(file);
    printf("wake replay frames=%d hits=%d status=%d\n", frames, hits, rc);
    return rc;
}
