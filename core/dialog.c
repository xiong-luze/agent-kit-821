#include "dialog.h"
#include <limits.h>
#include <string.h>

int dialog_init(dialog_t *d, const dialog_config_t *c, int64_t now_ms)
{
    if (!d || !c || c->frame_ms <= 0 || c->frame_ms > 100 ||
        c->vad_start <= 0 || c->vad_start > 32767 || c->vad_end < 0 ||
        c->vad_end >= c->vad_start || c->barge_ms < c->frame_ms ||
        c->barge_ms > 2000 || c->vad_hang_ms < c->frame_ms ||
        c->max_turn_ms < c->vad_hang_ms || c->reply_timeout_ms < c->frame_ms ||
        c->tail_quiet_ms < 0 || (c->wake_enabled && c->wake_hold_ms <= 0)) return -1;
    memset(d, 0, sizeof(*d));
    d->cfg = *c;
    d->state = c->wake_enabled ? DIALOG_SLEEP : DIALOG_LISTEN;
    d->awake_until_ms = now_ms + c->wake_hold_ms;
    return 0;
}

static unsigned start_turn(dialog_t *d, int64_t now_ms)
{
    d->state = DIALOG_SEND;
    d->voiced = d->silent = 0;
    d->turn_start_ms = now_ms;
    d->awake_until_ms = now_ms + d->cfg.wake_hold_ms;
    return DIALOG_START;
}

static unsigned finish_turn(dialog_t *d, int64_t now_ms)
{
    d->state = DIALOG_LISTEN;
    d->voiced = d->silent = 0;
    d->awake_until_ms = now_ms + d->cfg.wake_hold_ms;
    return DIALOG_FINISHED;
}

unsigned dialog_step(dialog_t *d, const dialog_input_t *in)
{
    const dialog_config_t *c = &d->cfg;
    switch (d->state) {
    case DIALOG_SLEEP:
        if (in->wake) return start_turn(d, in->now_ms) | DIALOG_WOKE;
        return 0;
    case DIALOG_LISTEN:
        if (c->wake_enabled && in->now_ms >= d->awake_until_ms) {
            d->state = DIALOG_SLEEP;
            d->voiced = 0;
            return DIALOG_SLEPT;
        }
        d->voiced = in->rms >= c->vad_start ? d->voiced + 1 : 0;
        if (d->voiced >= 3) return start_turn(d, in->now_ms);
        return 0;
    case DIALOG_SEND: {
        d->silent = in->rms < c->vad_end ? d->silent + 1 : 0;
        unsigned action = DIALOG_AUDIO;
        if (in->asr_final || in->reply_audio ||
            (int64_t)d->silent * c->frame_ms >= c->vad_hang_ms ||
            in->now_ms - d->turn_start_ms >= c->max_turn_ms) {
            d->state = DIALOG_WAIT;
            d->wait_start_ms = in->now_ms;
            d->voiced = 0;
            action |= DIALOG_END;
        }
        return action;
    }
    case DIALOG_WAIT:
    case DIALOG_PLAY: {
        if (in->reply_audio) d->state = DIALOG_PLAY;
        int threshold = in->playback_active ? c->vad_start + c->vad_start / 2 : c->vad_start;
        if (threshold > 32767) threshold = 32767;
        int confirm_ms = in->playback_active ? c->barge_ms : c->frame_ms * 3;
        d->voiced = !in->echo_likely && in->rms >= threshold ? d->voiced + 1 : 0;
        if ((int64_t)d->voiced * c->frame_ms >= confirm_ms)
            return start_turn(d, in->now_ms) | DIALOG_INTERRUPT;
        if (in->reply_done && !in->playback_active &&
            in->reply_quiet_ms >= c->tail_quiet_ms)
            return finish_turn(d, in->now_ms);
        if (!in->playback_active && in->reply_quiet_ms >= c->reply_timeout_ms &&
            in->now_ms - d->wait_start_ms >= c->reply_timeout_ms)
            return finish_turn(d, in->now_ms) | DIALOG_INTERRUPT;
        return 0;
    }
    }
    return 0;
}
