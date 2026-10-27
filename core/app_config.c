#include "app.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int envi(const char *key, int fallback)
{
    const char *value = getenv(key);
    if (!value) return fallback;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || end == value || *end || n < 0 || n > INT_MAX) return -1;
    return (int)n;
}

void app_cfg_defaults(app_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->sample_rate = 16000;
    c->frame_ms = 20;
    c->vad_start = envi("V821_VAD_START", 0);
    c->vad_end = envi("V821_VAD_END", 0);
    c->vad_hang_ms = envi("V821_VAD_HANG", 1000);
    c->guard_ms = envi("V821_GUARD_MS", 300);
    c->aec_tail_ms = envi("V821_AEC_TAIL", 300);
    c->aec_enable = 1;
    c->greet_enable = envi("V821_GREET", 1);
    c->greet_idle_ms = envi("V821_GREET_IDLE_MS", 60000);
    c->voice_trigger = envi("V821_VOICE_TRIGGER", 1);
    c->full_duplex = envi("V821_FULL_DUPLEX", 0);
    c->wake_enabled = envi("V821_WAKE_ENABLED", 1);
    c->wake_min_rms = envi("V821_WAKE_MIN_RMS", 350);
    c->wake_word = getenv("V821_WAKE_WORD");
    if (!c->wake_word) c->wake_word = "你好涂鸦";
    c->wake_model = getenv("V821_WAKE_MODEL");
    c->wake_dictionary = getenv("V821_WAKE_DICTIONARY");
    c->wake_threshold = 1e-25;
    const char *threshold = getenv("V821_WAKE_THRESHOLD");
    if (threshold) {
        char *end;
        errno = 0;
        c->wake_threshold = strtod(threshold, &end);
        if (errno || end == threshold || *end) c->wake_threshold = NAN;
    }
    c->wake_hold_ms = envi("V821_WAKE_HOLD_MS", 30000);
    c->barge_ms = envi("V821_BARGE_MS", 120);
    c->pre_roll_ms = envi("V821_PRE_ROLL_MS", 600);
}

int app_cfg_validate(const app_cfg_t *c)
{
    if (!c || c->sample_rate != 16000 || c->frame_ms != 20 ||
        c->vad_start < 0 || c->vad_start > 32767 || c->vad_end < 0 || c->vad_end > 32767 ||
        c->vad_hang_ms < 20 || c->vad_hang_ms > 10000 ||
        c->guard_ms < 0 || c->guard_ms > 2000 || c->aec_tail_ms < 20 || c->aec_tail_ms > 1000 ||
        c->full_duplex < 0 || c->full_duplex > 1 || c->wake_enabled < 0 || c->wake_enabled > 1 ||
        c->wake_min_rms < 0 || c->wake_min_rms > 32767 ||
        c->voice_trigger < 0 || c->voice_trigger > 1 || c->greet_enable < 0 || c->greet_enable > 1 ||
        c->greet_idle_ms < 0) {
        fprintf(stderr, "[配置] 无效的音频/VAD/模式参数\n");
        return -1;
    }
    if (!c->full_duplex) return 0;
    if (!c->aec_enable || !c->voice_trigger || c->pre_roll_ms < 20 || c->pre_roll_ms > 1000 ||
        c->pre_roll_ms % c->frame_ms || c->barge_ms < 40 || c->barge_ms > 1000 ||
        c->wake_hold_ms < 1000 || c->wake_hold_ms > 600000) {
        fprintf(stderr, "[配置] 全双工需要 AEC/语音输入；预录20..1000ms，打断40..1000ms\n");
        return -1;
    }
    if (c->wake_enabled && (!c->wake_word || !c->wake_word[0] || strlen(c->wake_word) > 96 ||
        !c->wake_model || !c->wake_model[0] || !c->wake_dictionary || !c->wake_dictionary[0] ||
        !isfinite(c->wake_threshold) || c->wake_threshold <= 0 || c->wake_threshold >= 1)) {
        fprintf(stderr, "[配置] 本地唤醒需要有效短语、模型目录和词典；AEC 单独验证可用 --no-wake\n");
        return -1;
    }
    return 0;
}
