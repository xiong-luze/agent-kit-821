#include "wake_word.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void wake_word_config_defaults(wake_word_config_t *config)
{
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->phrase = "你好涂鸦";
    config->threshold = 1e-25;
    config->activity_min_rms = 350;
}

#ifndef HAVE_POCKETSPHINX

int wake_word_start(const wake_word_config_t *config)
{
    (void)config;
    fprintf(stderr, "[唤醒] 当前构建未启用 PocketSphinx，本地唤醒不可用\n");
    return -1;
}
int wake_word_feed(const int16_t *pcm, size_t count) { (void)pcm; (void)count; return -1; }
int wake_word_poll(void) { return -1; }
int wake_word_set_enabled(int enabled) { (void)enabled; return -1; }
void wake_word_stop(void) {}

#else

#include <pthread.h>
#include <pocketsphinx.h>

#define WAKE_FRAME_MAX 320
#define WAKE_QUEUE_FRAMES 32

typedef struct {
    int16_t pcm[WAKE_FRAME_MAX];
    size_t count;
} wake_frame_t;

static struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    int started, stopping, failed, enabled, hit;
    unsigned long epoch;
    int reset, trace, activity_min_rms;
    char *initial_cmn;
    unsigned high_water;
    unsigned long processed_frames;
    long long last_infer_ms, max_infer_ms;
    wake_frame_t queue[WAKE_QUEUE_FRAMES];
    size_t head, used;
    ps_decoder_t *decoder;
    char phrase[128];
} state = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};

static long long timestamp_ms(clockid_t clock)
{
    struct timespec now;
    clock_gettime(clock, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int decoder_reset(void)
{
    long long before = state.trace ? timestamp_ms(CLOCK_MONOTONIC) : 0;
    int rc = -1;
    if (ps_end_utt(state.decoder) >= 0 && ps_set_cmn(state.decoder, state.initial_cmn) >= 0)
        rc = ps_start_utt(state.decoder);
    if (state.trace) {
        long long elapsed = timestamp_ms(CLOCK_MONOTONIC) - before;
        if (elapsed >= 40)
            fprintf(stderr, "[关键词时序] phase=reset wall_ms=%lld\n", elapsed);
    }
    return rc;
}

static int keyword_detected(void)
{
    /* KWS display summaries may join repeated detections with spaces. Compare
     * structured segments instead, then reset after any match so result history
     * cannot grow indefinitely when a summary contains multiple copies. */
    for (ps_seg_t *segment = ps_seg_iter(state.decoder); segment;
         segment = ps_seg_next(segment)) {
        const char *word = ps_seg_word(segment);
        if (word && !strcmp(word, state.phrase)) {
            ps_seg_free(segment);
            return 1;
        }
    }
    return 0;
}

static void worker_failed(void)
{
    pthread_mutex_lock(&state.mutex);
    state.failed = 1;
    state.used = 0;
    pthread_mutex_unlock(&state.mutex);
    fprintf(stderr, "[唤醒] 推理失败，停止关键词检测\n");
}

static void *wake_worker(void *unused)
{
    (void)unused;
    unsigned quiet_samples = 0;
    unsigned long channel_restores = 0;
    for (;;) {
        wake_frame_t frame;
        unsigned long epoch;
        int reset;
        pthread_mutex_lock(&state.mutex);
        while (!state.stopping && !state.failed && !state.reset && !state.used)
            pthread_cond_wait(&state.condition, &state.mutex);
        if (state.stopping || state.failed) {
            pthread_mutex_unlock(&state.mutex);
            return NULL;
        }
        reset = state.reset;
        state.reset = 0;
        epoch = state.epoch;
        if (reset) frame.count = 0;
        else {
            frame = state.queue[state.head];
            state.head = (state.head + 1) % WAKE_QUEUE_FRAMES;
            state.used--;
        }
        pthread_mutex_unlock(&state.mutex);

        if (reset) {
            quiet_samples = 0;
            if (decoder_reset() < 0) { worker_failed(); return NULL; }
            continue;
        }
        long long before = state.trace ? timestamp_ms(CLOCK_MONOTONIC) : 0;
        long long cpu_before = state.trace ? timestamp_ms(CLOCK_THREAD_CPUTIME_ID) : 0;
        /* Live CMN includes nonzero-energy background frames. A long quiet
         * stream can therefore replace the initial channel baseline with noise and
         * change both false matches and later speech scores. Keep the initial
         * baseline during quiet without truncating any audio or keyword path. */
        if (state.activity_min_rms > 0) {
            int64_t energy = 0;
            for (size_t i = 0; i < frame.count; i++) energy += (int64_t)frame.pcm[i] * frame.pcm[i];
            int64_t floor = (int64_t)state.activity_min_rms * state.activity_min_rms * (int64_t)frame.count;
            if (energy < floor) quiet_samples += (unsigned)frame.count;
            else quiet_samples = 0;
            if (quiet_samples >= 16000) {
                if (ps_set_cmn(state.decoder, state.initial_cmn) < 0) { worker_failed(); return NULL; }
                quiet_samples = 0;
                channel_restores++;
                if (state.trace && channel_restores % 60 == 0)
                    fprintf(stderr, "[关键词诊断] channel_restores=%lu\n", channel_restores);
            }
        }
        if (ps_process_raw(state.decoder, frame.pcm, frame.count, 0, 0) < 0) {
            worker_failed();
            return NULL;
        }
        int detected = keyword_detected();
        long long elapsed = state.trace ? timestamp_ms(CLOCK_MONOTONIC) - before : 0;
        long long cpu = state.trace ? timestamp_ms(CLOCK_THREAD_CPUTIME_ID) - cpu_before : 0;
        pthread_mutex_lock(&state.mutex);
        state.processed_frames++;
        state.last_infer_ms = elapsed;
        if (elapsed > state.max_infer_ms) state.max_infer_ms = elapsed;
        unsigned queued = (unsigned)state.used;
        if (detected && state.epoch == epoch && state.enabled && !state.stopping)
            state.hit = 1;
        pthread_mutex_unlock(&state.mutex);
        if (state.trace && elapsed >= 40)
            fprintf(stderr, "[关键词时序] phase=infer wall_ms=%lld cpu_ms=%lld queued=%u\n",
                    elapsed, cpu, queued);
        if (detected) quiet_samples = 0;
        if (detected && decoder_reset() < 0) { worker_failed(); return NULL; }
    }
}

static int valid_phrase(const char *phrase)
{
    if (!phrase || !*phrase || strlen(phrase) >= sizeof(state.phrase)) return 0;
    for (const unsigned char *p = (const unsigned char *)phrase; *p; p++)
        if (*p <= 32 || *p == 127) return 0;
    return 1;
}

int wake_word_start(const wake_word_config_t *config)
{
    if (!config || !config->model_dir || !*config->model_dir ||
        !config->dictionary || !*config->dictionary || !valid_phrase(config->phrase) ||
        !isfinite(config->threshold) || config->threshold <= 0 || config->threshold >= 1 ||
        config->activity_min_rms < 0 || config->activity_min_rms > 32767)
        return -1;
    /* Lifecycle calls belong to one owner thread, as do DSP init/deinit. */
    pthread_mutex_lock(&state.mutex);
    int already_started = state.started;
    pthread_mutex_unlock(&state.mutex);
    if (already_started) return -1;

    ps_config_t *ps_config = ps_config_init(NULL);
    if (!ps_config) return -1;
    int configured = ps_config_set_str(ps_config, "hmm", config->model_dir) &&
        ps_config_set_str(ps_config, "dict", config->dictionary) &&
        ps_config_set_str(ps_config, "lm", NULL) &&
        ps_config_set_str(ps_config, "keyphrase", config->phrase) &&
        ps_config_set_float(ps_config, "samprate", 16000) &&
        ps_config_set_float(ps_config, "kws_threshold", config->threshold) &&
        ps_config_set_str(ps_config, "loglevel", "WARN");
    ps_decoder_t *decoder = configured ? ps_init(ps_config) : NULL;
    ps_config_free(ps_config);
    if (!decoder) {
        fprintf(stderr, "[唤醒] 模型/词典初始化失败：%s\n", config->phrase);
        return -1;
    }
    /* PocketSphinx can initialize an empty KWS search after warning that the
     * phrase is absent. Treat a mismatched custom dictionary as a hard error. */
    char *pronunciation = ps_lookup_word(decoder, config->phrase);
    if (!pronunciation || !*pronunciation) {
        fprintf(stderr, "[唤醒] 词典不包含配置的关键词：%s\n", config->phrase);
        free(pronunciation);
        ps_free(decoder);
        return -1;
    }
    free(pronunciation);
    const char *mean = ps_get_cmn(decoder, 0);
    char *initial_cmn = mean ? malloc(strlen(mean) + 1) : NULL;
    if (!initial_cmn) { ps_free(decoder); return -1; }
    strcpy(initial_cmn, mean);
    if (ps_start_utt(decoder) < 0) { free(initial_cmn); ps_free(decoder); return -1; }

    pthread_mutex_lock(&state.mutex);
    state.decoder = decoder;
    state.initial_cmn = initial_cmn;
    state.activity_min_rms = config->activity_min_rms;
    strcpy(state.phrase, config->phrase);
    state.head = state.used = 0;
    state.stopping = state.failed = state.hit = state.reset = 0;
    state.enabled = 1;
    const char *trace = getenv("V821_VOICE_TRACE");
    state.trace = trace && !strcmp(trace, "1");
    state.high_water = 0;
    state.processed_frames = 0;
    state.last_infer_ms = state.max_infer_ms = 0;
    state.epoch++;
    int rc = pthread_create(&state.thread, NULL, wake_worker, NULL);
    state.started = rc == 0;
    pthread_mutex_unlock(&state.mutex);
    if (rc) {
        fprintf(stderr, "[唤醒] 工作线程创建失败：%s\n", strerror(rc));
        ps_end_utt(decoder);
        ps_free(decoder);
        free(initial_cmn);
        state.initial_cmn = NULL;
        state.decoder = NULL;
        return -1;
    }
    printf("[唤醒] 本地关键词：%s（独立线程，阈值 %.3g）\n", state.phrase, config->threshold);
    return 0;
}

int wake_word_feed(const int16_t *pcm, size_t count)
{
    if (!pcm || count == 0 || count > WAKE_FRAME_MAX) return -1;
    pthread_mutex_lock(&state.mutex);
    int rc = 0;
    if (!state.started || state.stopping || state.failed) rc = -1;
    else if (!state.enabled) rc = 0;
    else if (state.used == WAKE_QUEUE_FRAMES) {
        state.failed = 1;
        state.used = 0;
        pthread_cond_signal(&state.condition);
        fprintf(stderr, "[唤醒] 输入队列溢出，关键词检测已停止（需要检查实时预算）\n");
        fprintf(stderr, "[关键词诊断] overflow=1 high_water=%u processed=%lu last_ms=%lld max_ms=%lld\n",
                state.high_water, state.processed_frames, state.last_infer_ms, state.max_infer_ms);
        rc = -1;
    } else {
        wake_frame_t *frame = &state.queue[(state.head + state.used) % WAKE_QUEUE_FRAMES];
        memcpy(frame->pcm, pcm, count * sizeof(*pcm));
        frame->count = count;
        state.used++;
        if (state.used > state.high_water) state.high_water = (unsigned)state.used;
        pthread_cond_signal(&state.condition);
    }
    pthread_mutex_unlock(&state.mutex);
    return rc;
}

int wake_word_poll(void)
{
    pthread_mutex_lock(&state.mutex);
    int result = !state.started || state.failed || state.stopping ? -1 : state.hit;
    state.hit = 0;
    pthread_mutex_unlock(&state.mutex);
    return result;
}

int wake_word_set_enabled(int enabled)
{
    pthread_mutex_lock(&state.mutex);
    int rc = 0;
    if (!state.started || state.failed || state.stopping) rc = -1;
    else if (state.enabled != !!enabled) {
        state.enabled = !!enabled;
        state.hit = 0;
        state.head = state.used = 0;
        state.epoch++;
        state.reset = 1;
        pthread_cond_signal(&state.condition);
    }
    pthread_mutex_unlock(&state.mutex);
    return rc;
}

void wake_word_stop(void)
{
    pthread_mutex_lock(&state.mutex);
    int started = state.started;
    if (started) {
        state.stopping = 1;
        pthread_cond_signal(&state.condition);
    }
    pthread_mutex_unlock(&state.mutex);
    if (!started) return;
    pthread_join(state.thread, NULL);
    ps_end_utt(state.decoder);
    ps_free(state.decoder);
    free(state.initial_cmn);
    pthread_mutex_lock(&state.mutex);
    state.decoder = NULL;
    state.initial_cmn = NULL;
    state.started = state.stopping = state.failed = state.enabled = state.hit = state.reset = 0;
    state.head = state.used = 0;
    pthread_mutex_unlock(&state.mutex);
}

#endif
