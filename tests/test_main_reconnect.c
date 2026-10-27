/* Exercise actual product orchestration; cloud and hardware are replaced.
 * Fixture credentials never reach a real SDK or network. */
#include <assert.h>
#include <time.h>
int test_pause(const struct timespec *, struct timespec *);
int test_clock(clockid_t, struct timespec *);
#define nanosleep test_pause
#define clock_gettime test_clock
#define main product_main
#include "../app/main.c"
#undef main
#undef nanosleep
#undef clock_gettime

static int connects, closes, runs, starts, stops, sleeps, ui_active, live, mcp_live;
static int connect_failures, retry_runs, fatal, full_mode;
static long delays[16];
static int retry_reason = -2, sustained_run_ms, resets;
static int64_t fake_now_ms;
static int board_init(void) { return 0; }
static int audio_start(int rate) { assert(rate == 16000); return 0; }
static void audio_stop(void) {}
static const bsp_t board = {.name="fixture", .caps=BSP_CAP_AUDIO_DUPLEX,
    .init=board_init, .audio_start=audio_start, .audio_stop=audio_stop};
const bsp_t *bsp_get(void) { return &board; }
int ui_start(void) { ui_active = 1; return 0; }
void ui_stop(void) { ui_active = 0; }
void ui_set_status(const char *s) { assert(ui_active && s); }
void ui_set_user(const char *s) { (void)s; }
void ui_ai_begin(void) {}
void ui_ai_append(const char *s) { (void)s; }
void ui_set_emotion(const char *s) { (void)s; }
void ui_show_qr(const unsigned char *s) { (void)s; }
int dsp_init(int rate, int frame, int tail, int enable)
{ assert(rate == 16000 && frame == 320 && tail > 0 && enable); return 0; }
void dsp_deinit(void) {}
int dsp_reset(void) { resets++; return 0; }
int ringbuf_init(int rate, int seconds) { assert(rate == 16000 && seconds > 0); return 0; }
void ringbuf_clear(void) {}
void ringbuf_free(void) {}
void app_tune(const app_cfg_t *c) { (void)c; assert(0); }
void presence_cfg_defaults(presence_cfg_t *c) { memset(c, 0, sizeof(*c)); }
void presence_tune(const presence_cfg_t *c) { (void)c; assert(0); }
int iot_init_default(void) { return 0; }
int prov_load(prov_creds_t *c) { (void)c; return 0; }
void prov_erase(void) { assert(0); }
int prov_run_qr(const char *a, const char *b, const char *c, prov_creds_t *d,
                 void (*e)(const char *), void (*f)(const unsigned char *))
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; assert(0); return -1; }
int session_connect(const char *a, const char *b, const char *c, const session_ui_t *ui)
{
    assert(a && b && c && ui && ui_active && !live && !mcp_live);
    live = 1; /* A failed connect may own partially initialized resources. */
    assert(connects < 15);
    return connects++ < connect_failures ? -1 : 0;
}
void session_close(void) { assert(live && !mcp_live && ui_active); live = 0; closes++; }
tai_ctx_t *session_ctx(void) { assert(live); return (tai_ctx_t *)&live; }
int mcp_start(tai_ctx_t *ctx, void (*status)(const char *))
{ assert(ctx && status && live && !mcp_live); mcp_live = 1; starts++; return 0; }
void mcp_stop(void) { assert(mcp_live && live); mcp_live = 0; stops++; }
int app_run_duplex(const app_cfg_t *c)
{
    assert(c->full_duplex && live && mcp_live && ui_active);
    runs++;
    fake_now_ms += sustained_run_ms;
    return fatal == 2 ? -3 : fatal ? -1 : runs <= retry_runs ? retry_reason : 0;
}
void app_run(const app_cfg_t *c) { assert(!c->full_duplex && live); runs++; }
int test_pause(const struct timespec *duration, struct timespec *remaining)
{
    (void)remaining;
    assert(ui_active && !live && !mcp_live && sleeps < 16);
    delays[sleeps++] = duration->tv_sec;
    fake_now_ms += duration->tv_sec * 1000;
    return 0;
}
int test_clock(clockid_t clock, struct timespec *out)
{
    assert(clock == CLOCK_MONOTONIC);
    out->tv_sec = fake_now_ms / 1000;
    out->tv_nsec = fake_now_ms % 1000 * 1000000;
    return 0;
}
static void run_case(int failures, int retries, int terminal, int full)
{
    connects=closes=runs=starts=stops=sleeps=ui_active=live=mcp_live=0;
    resets=0; fake_now_ms=1000;
    connect_failures=failures; retry_runs=retries; fatal=terminal; full_mode=full;
    char *args[]={"fixture", "fixture-device", "fixture-secret", "fixture-local",
                  full_mode ? "--full-duplex" : "--half-duplex", "--no-greet", "--no-wake", NULL};
    int rc=product_main(7,args);
    assert(rc == (terminal ? 1 : 0));
    assert(!ui_active && !live && !mcp_live);
    int expected_runs = terminal == 2 ? 4 : retries + 1;
    assert(connects == failures + expected_runs && closes == connects);
    assert(runs == expected_runs && starts == runs && stops == runs);
    assert(sleeps == failures + (terminal == 2 ? 3 : retries));
    if (full) assert(resets == runs);
}
int main(void)
{
    run_case(2,1,0,1);
    assert(delays[0] == 1 && delays[1] == 2 && delays[2] == 1);
    run_case(5,0,0,1);
    assert(delays[0] == 1 && delays[1] == 2 && delays[2] == 4 && delays[3] == 5 && delays[4] == 5);
    run_case(0,0,1,1); /* Device/model failures must not loop forever. */
    run_case(0,0,0,0); /* Original half-duplex lifecycle stays single-attempt. */
    retry_reason=-3;
    run_case(0,2,0,1); /* Isolated XRUN rebuilds both DSP and connection. */
    run_case(0,0,2,1); /* At most three rapid rebuilds; fourth XRUN terminates. */
    sustained_run_ms=31000;
    run_case(0,4,0,1); /* A stable30s run replenishes the recovery budget. */
    puts("product reconnect: cleanup, bounded backoff and terminal failure passed");
    return 0;
}
