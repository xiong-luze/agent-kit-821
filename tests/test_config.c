#include "core/app.h"
#include "core/ringbuf.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    app_cfg_t c;
    app_cfg_defaults(&c);
    assert(!strcmp(c.wake_word, "你好涂鸦"));
    assert(app_cfg_validate(&c) == 0);
    c.full_duplex = 1;
    assert(app_cfg_validate(&c) != 0); /* Missing model must not become VAD wake. */
    c.wake_model = "model"; c.wake_dictionary = "dict";
    assert(app_cfg_validate(&c) == 0);
    c.aec_enable = 0;
    assert(app_cfg_validate(&c) != 0);
    c.aec_enable = 1; c.pre_roll_ms = 1020;
    assert(app_cfg_validate(&c) != 0);
    setenv("V821_FULL_DUPLEX", "junk", 1);
    app_cfg_defaults(&c);
    assert(app_cfg_validate(&c) != 0);
    unsetenv("V821_FULL_DUPLEX");
    const char *invalid_floor[] = {"-1", "32768", "bad"};
    for (unsigned i = 0; i < sizeof(invalid_floor) / sizeof(*invalid_floor); i++) {
        setenv("V821_WAKE_MIN_RMS", invalid_floor[i], 1);
        app_cfg_defaults(&c); assert(app_cfg_validate(&c) != 0);
    }
    setenv("V821_WAKE_MIN_RMS", "0", 1);
    app_cfg_defaults(&c); assert(c.wake_min_rms == 0 && app_cfg_validate(&c) == 0);
    unsetenv("V821_WAKE_MIN_RMS");
    app_cfg_defaults(&c); assert(c.wake_min_rms == 350);
    assert(ringbuf_avail() == 0);
    int16_t samples[] = {1, 2, 3, 4}, out[5];
    assert(ringbuf_push(samples, 4) == 0);
    assert(ringbuf_pop_frame(out, 5) == 0);
    for (int i = 0; i < 5; i++) assert(out[i] == 0);
    assert(ringbuf_init(0, 1) != 0);
    assert(ringbuf_init(4, 1) == 0);
    assert(ringbuf_push(samples, 4) == 3);
    assert(ringbuf_pop_frame(out, 5) == 3);
    assert(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 0 && out[4] == 0);
    ringbuf_free();
    assert(ringbuf_avail() == 0);
    puts("configuration failures and ring overflow checks passed");
    return 0;
}
