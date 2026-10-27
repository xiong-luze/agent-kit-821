#include "core/dialog.h"
#include "core/pcm_history.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static dialog_input_t input(int64_t now, int rms)
{
    dialog_input_t in = {0};
    in.now_ms = now;
    in.rms = rms;
    return in;
}

int main(void)
{
    dialog_config_t cfg = { .frame_ms = 20, .wake_enabled = 1, .wake_hold_ms = 30000,
        .vad_start = 1000, .vad_end = 600, .vad_hang_ms = 200, .barge_ms = 120,
        .max_turn_ms = 15000, .reply_timeout_ms = 30000, .tail_quiet_ms = 800 };
    dialog_t d;
    assert(dialog_init(&d, &cfg, 0) == 0);
    for (int i = 0; i < 100; i++) {
        dialog_input_t in = input(i * 20, 5000);
        assert(dialog_step(&d, &in) == 0); /* Loud sound is not a wake phrase. */
    }
    assert(d.state == DIALOG_SLEEP);
    dialog_input_t in = input(2000, 0);
    in.wake = 1;
    unsigned action = dialog_step(&d, &in);
    assert((action & (DIALOG_START | DIALOG_WOKE)) == (DIALOG_START | DIALOG_WOKE));
    assert(d.state == DIALOG_SEND);
    for (int i = 1; i < 10; i++) {
        in = input(2000 + i * 20, 0);
        assert(dialog_step(&d, &in) == DIALOG_AUDIO);
    }
    in = input(2200, 0);
    action = dialog_step(&d, &in);
    assert((action & (DIALOG_AUDIO | DIALOG_END)) == (DIALOG_AUDIO | DIALOG_END));
    assert(d.state == DIALOG_WAIT);

    /* Residual playback below the stricter barge threshold cannot interrupt. */
    for (int i = 0; i < 30; i++) {
        in = input(2220 + i * 20, 1200);
        in.reply_audio = in.playback_active = 1;
        assert(dialog_step(&d, &in) == 0);
    }
    for (int i = 0; i < 8; i++) {
        in = input(2820 + i * 20, 5000);
        in.reply_audio = in.playback_active = in.echo_likely = 1;
        assert(dialog_step(&d, &in) == 0); /* Loud explained echo isn't near speech. */
    }
    for (int i = 0; i < 5; i++) {
        in = input(2980 + i * 20, 2000);
        in.reply_audio = in.playback_active = 1;
        assert(dialog_step(&d, &in) == 0);
    }
    in = input(3080, 2000);
    in.reply_audio = in.playback_active = 1;
    action = dialog_step(&d, &in);
    assert((action & (DIALOG_INTERRUPT | DIALOG_START)) == (DIALOG_INTERRUPT | DIALOG_START));
    assert(d.state == DIALOG_SEND); /* No cloud confirmation is needed here. */
    in = input(3100, 0);
    in.asr_final = 1;
    assert(dialog_step(&d, &in) & DIALOG_END);
    in = input(3120, 0);
    in.reply_audio = in.reply_done = in.playback_active = 1;
    in.reply_quiet_ms = 900;
    assert(dialog_step(&d, &in) == 0); /* Sound card still has speech queued. */
    in.playback_active = 0;
    in.reply_quiet_ms = 100;
    assert(dialog_step(&d, &in) == 0); /* A second MCP event may follow. */
    in.now_ms = 40000;
    in.playback_active = 1;
    in.reply_quiet_ms = 35000;
    assert(dialog_step(&d, &in) == 0); /* A long queued reply is still playing. */
    in.now_ms = 40020;
    in.playback_active = 0;
    in.reply_quiet_ms = 900;
    assert(dialog_step(&d, &in) & DIALOG_FINISHED);
    assert(d.state == DIALOG_LISTEN);
    in = input(71000, 0);
    assert(dialog_step(&d, &in) == DIALOG_SLEPT);
    assert(d.state == DIALOG_SLEEP);

    cfg.wake_enabled = 0;
    assert(dialog_init(&d, &cfg, 0) == 0);
    for (int i = 1; i <= 3; i++) {
        in = input(i * 20, 1500);
        action = dialog_step(&d, &in);
        assert((action & DIALOG_START) == (i == 3 ? DIALOG_START : 0));
    }
    in = input(15100, 1500);
    assert(dialog_step(&d, &in) & DIALOG_END); /* Hard duration cap. */
    in = input(45200, 0);
    in.reply_quiet_ms = 30100;
    action = dialog_step(&d, &in);
    assert((action & (DIALOG_INTERRUPT | DIALOG_FINISHED)) ==
           (DIALOG_INTERRUPT | DIALOG_FINISHED));
    assert(!(action & DIALOG_START));
    cfg.frame_ms = 0;
    assert(dialog_init(&d, &cfg, 0) != 0);

    pcm_history_t h;
    assert(pcm_history_init(&h, 5) == 0);
    int16_t a[] = {1, 2, 3}, b[] = {4, 5, 6, 7}, result[5];
    pcm_history_push(&h, a, 3);
    pcm_history_push(&h, b, 4);
    assert(pcm_history_read(&h, result, 5) == 5);
    int16_t expected[] = {3, 4, 5, 6, 7};
    assert(!memcmp(result, expected, sizeof(expected)));
    pcm_history_clear(&h);
    assert(pcm_history_read(&h, result, 5) == 0);
    pcm_history_free(&h);
    puts("dialog wake gate, barge-in, tail, timeout and PCM pre-roll checks passed");
    return 0;
}
