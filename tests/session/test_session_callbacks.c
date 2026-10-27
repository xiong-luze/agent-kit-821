/* Real session callback/queue integration, with only the wire transport replaced. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <mbedtls/base64.h>
#include "../../core/jsonx.h"
static int test_json_get_string_esc(const char *, const char *, char *, size_t);
#define json_get_string_esc test_json_get_string_esc
#include "../../service/session.c"
#undef json_get_string_esc

static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tx_cond = PTHREAD_COND_INITIALIZER;
static int starts, breaks, chunks, ends, tools, tool_calls, images;
static int block_image, image_entered, release_image;
static int fake_context;
static int sdk_disconnects, sdk_deinits;
static pthread_mutex_t parse_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t parse_cond = PTHREAD_COND_INITIALIZER;
static int pause_ui_parse, parse_entered, parse_released;
static int ui_clears;
static char displayed[128];
static int stall_audio, audio_entered, release_audio, sequence_used;
static int16_t sent_sequence[160];

static int test_json_get_string_esc(const char *json, const char *key, char *out, size_t size)
{
    pthread_mutex_lock(&parse_mutex);
    if (pause_ui_parse && !strcmp(key, "content")) {
        parse_entered = 1;
        pthread_cond_broadcast(&parse_cond);
        while (!parse_released) pthread_cond_wait(&parse_cond, &parse_mutex);
    }
    pthread_mutex_unlock(&parse_mutex);
    return json_get_string_esc(json, key, out, size);
}

static void ui_begin(void) { displayed[0] = '\0'; ui_clears++; }
static void ui_append(const char *text)
{
    size_t used = strlen(displayed);
    snprintf(displayed + used, sizeof(displayed) - used, "%s", text);
}

/* Authentication is not exercised by callback tests. Keep its decoder stub
 * explicit instead of linking a second SDK/mbedTLS build into this fixture. */
int mbedtls_base64_decode(unsigned char *dst, size_t size, size_t *out_size,
                          const unsigned char *src, size_t src_size)
{
    (void)dst; (void)size; (void)out_size; (void)src; (void)src_size;
    return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
}

const pal_t *tai_pal_posix(void) { return NULL; }
iot_client_t *iot_client_init(const iot_client_config_t *config) { (void)config; return NULL; }
void iot_client_deinit(iot_client_t *client) { (void)client; }
int iot_client_get_session_token(iot_client_t *client, const char *agent, char *token, size_t size)
{ (void)client; (void)agent; (void)token; (void)size; return -1; }
size_t tai_ctx_size(void) { return 1; }
tai_ctx_t *tai_ctx_init(void *memory, const tai_config_t *config)
{ (void)config; return memory; }
void tai_disconnect(tai_ctx_t *ctx)
{ assert(ctx == (tai_ctx_t *)&fake_context); sdk_disconnects++; }
void tai_ctx_deinit(tai_ctx_t *ctx)
{ assert(ctx == (tai_ctx_t *)&fake_context && sdk_disconnects == 1); sdk_deinits++; }
int tai_connect(tai_ctx_t *ctx) { (void)ctx; return 0; }
int tai_send_audio_start(tai_ctx_t *ctx, uint8_t codec, uint8_t channels, uint8_t bits, uint32_t rate)
{
    (void)ctx;
    assert(codec == TAI_AUDIO_PCM && channels == 1 && bits == 16 && rate == 16000);
    pthread_mutex_lock(&tx_mutex);
    starts++;
    pthread_cond_broadcast(&tx_cond);
    pthread_mutex_unlock(&tx_mutex);
    return 0;
}
int tai_send_audio_chunk(tai_ctx_t *ctx, const uint8_t *pcm, size_t size)
{
    (void)ctx; assert(size);
    pthread_mutex_lock(&tx_mutex);
    audio_entered = 1;
    pthread_cond_broadcast(&tx_cond);
    while (stall_audio && !release_audio) pthread_cond_wait(&tx_cond, &tx_mutex);
    if (stall_audio) {
        assert(sequence_used < 160);
        memcpy(&sent_sequence[sequence_used++], pcm, sizeof(int16_t));
    }
    chunks++;
    pthread_mutex_unlock(&tx_mutex);
    return 0;
}
int tai_send_audio_end(tai_ctx_t *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&tx_mutex);
    ends++;
    pthread_cond_broadcast(&tx_cond);
    pthread_mutex_unlock(&tx_mutex);
    return 0;
}
int tai_send_text(tai_ctx_t *ctx, const char *text, size_t size)
{ (void)ctx; (void)text; (void)size; return 0; }
int tai_chat_break(tai_ctx_t *ctx) { (void)ctx; breaks++; return 0; }
void mcp_handle_generation(tai_ctx_t *ctx, const char *json, uint64_t generation)
{
    (void)ctx;
    (void)generation;
    session_state_t state;
    session_snapshot(&state);  /* MCP/network execution must not hold state lock. */
    char method[64] = {0};
    json_get_string(json, "method", method, sizeof(method));
    tools++;
    tool_calls += !strcmp(method, "tools/call");
}
void mcp_handle(tai_ctx_t *ctx, const char *json) { mcp_handle_generation(ctx, json, 0); }

int tai_send_image_with_text(tai_ctx_t *ctx, const char *text, size_t text_size,
                             const uint8_t *jpeg, size_t size, uint8_t format,
                             uint16_t width, uint16_t height)
{
    (void)ctx; (void)text; (void)text_size; (void)jpeg;
    assert(size && format == TAI_IMG_JPEG && width && height);
    pthread_mutex_lock(&tx_mutex);
    images++;
    image_entered = 1;
    pthread_cond_broadcast(&tx_cond);
    while (block_image && !release_image) pthread_cond_wait(&tx_cond, &tx_mutex);
    pthread_mutex_unlock(&tx_mutex);
    return 0;
}

static void wait_starts(int count)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&tx_mutex);
    while (starts < count) assert(!pthread_cond_timedwait(&tx_cond, &tx_mutex, &until));
    pthread_mutex_unlock(&tx_mutex);
}

static void wait_ends(int count)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&tx_mutex);
    while (ends < count) assert(!pthread_cond_timedwait(&tx_cond, &tx_mutex, &until));
    pthread_mutex_unlock(&tx_mutex);
}

static uint64_t current_generation(void)
{
    pthread_mutex_lock(&s_state_mutex);
    uint64_t value = s_guard.generation;
    pthread_mutex_unlock(&s_state_mutex);
    return value;
}

static void wait_event_waiter(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&s_event_mutex);
    while (!s_event_waiters) assert(!pthread_cond_timedwait(&s_event_cond, &s_event_mutex, &until));
    pthread_mutex_unlock(&s_event_mutex);
}

typedef struct { uint64_t generation; int result; } photo_job_t;
static void *send_photo(void *context)
{
    photo_job_t *job = context;
    const uint8_t jpeg[] = {0xff, 0xd8, 0xff, 0xd9};
    job->result = session_send_photo_for_generation(job->generation, "test", jpeg, sizeof(jpeg), 1, 1);
    return NULL;
}

static void wait_for_image(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&tx_mutex);
    while (!image_entered) assert(!pthread_cond_timedwait(&tx_cond, &tx_mutex, &until));
    pthread_mutex_unlock(&tx_mutex);
}

static void asr(const char *id)
{
    const char json[] = "{\"bizType\":\"ASR\",\"data\":{\"text\":\"test\"},\"eof\":1}";
    tai_text_msg_t text = {.event_id = id, .text = json, .len = sizeof(json) - 1,
                          .stream_flag = TAI_STREAM_START};
    on_text(s_ctx, &text, NULL);
}

static void event(const char *id, int type)
{
    tai_event_msg_t message = {.event_id = id, .event_type = (uint16_t)type};
    on_event(s_ctx, &message, NULL);
}

static void mcp(const char *id, const char *method)
{
    char json[192];
    int size = snprintf(json, sizeof(json), "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"id\":1}", method);
    assert(size > 0 && (size_t)size < sizeof(json));
    tai_event_msg_t message = {.event_id = id, .event_type = TAI_EVT_MCP_CMD,
                              .data = (const uint8_t *)json, .len = (size_t)size};
    on_event(s_ctx, &message, NULL);
}

static void nlg(const char *id, const char *content)
{
    char json[192];
    int size = snprintf(json, sizeof(json), "{\"bizType\":\"NLG\",\"data\":{\"content\":\"%s\"}}", content);
    assert(size > 0 && (size_t)size < sizeof(json));
    tai_text_msg_t message = {.event_id = id, .text = json, .len = (size_t)size,
                             .stream_flag = TAI_STREAM_MIDDLE};
    on_text(s_ctx, &message, NULL);
}

static void *late_nlg(void *unused)
{
    (void)unused;
    nlg("mcp-continuation", "late old answer");
    return NULL;
}

static void wait_parse(void)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&parse_mutex);
    while (!parse_entered) assert(!pthread_cond_timedwait(&parse_cond, &parse_mutex, &until));
    pthread_mutex_unlock(&parse_mutex);
}

static void audio(const char *id)
{
    const union { int16_t alignment; uint8_t bytes[9]; } packet = {
        .bytes = {0xab, 100, 0, 0x9c, 0xff, 0xc8, 0, 0x38, 0xff}
    };
    tai_audio_msg_t message = {.event_id = id, .data = packet.bytes + 1,
        .len = 8, .codec = TAI_AUDIO_PCM, .sample_rate = 16000,
        .stream_flag = TAI_STREAM_START};
    on_audio(s_ctx, &message, NULL);
}

static void *late_audio(void *unused)
{
    (void)unused;
    for (int i = 0; i < 2000; i++) audio("old");
    return NULL;
}

int main(void)
{
    assert(ringbuf_init(16000, 1) == 0);
    s_ctx = (tai_ctx_t *)&fake_context;
    s_ui.on_ai_text = ui_append;
    s_ui.on_turn_begin = ui_begin;
    assert(session_duplex_enable() == 0);
    assert(session_audio_start() == -EPERM);
    assert(session_audio_end() == -EPERM);
    assert(session_send_text("must not bypass the async event owner") == -EPERM);
    mcp("", "initialize");
    mcp("", "tools/list");
    mcp("", "notifications/initialized");
    mcp("pre-wake", "tools/call");
    assert(tools == 3 && !tool_calls);
    assert(session_post_start() == 0);
    wait_starts(1);
    asr("old");
    assert(session_post_end() == 0);
    wait_ends(1);
    audio("old");
    session_state_t state;
    session_snapshot(&state);
    assert(state.got_audio && state.asr_final && ringbuf_avail() == 4);

    pthread_t producer;
    assert(!pthread_create(&producer, NULL, late_audio, NULL));
    assert(session_post_interrupt() == 0);
    assert(!pthread_join(producer, NULL));
    assert(!ringbuf_avail());
    assert(session_post_start() == 0);
    wait_starts(2);
    asr("old");
    audio("old");
    event("old", TAI_EVT_END);
    mcp("old", "tools/call");
    assert(tools == 3 && !tool_calls);
    audio("new");  /* No fresh ASR yet: must fail closed. */
    session_snapshot(&state);
    assert(!state.got_audio && !state.asr_final && !state.turn_done && !ringbuf_avail());

    asr("new");
    assert(session_post_end() == 0);
    wait_ends(2);
    audio("new");
    event("old", TAI_EVT_END);
    session_snapshot(&state);
    assert(state.got_audio && state.asr_final && !state.turn_done && ringbuf_avail() == 4);
    event("new", TAI_EVT_END);
    session_snapshot(&state);
    assert(state.turn_done);
    event("mcp-continuation", TAI_EVT_START);
    mcp("mcp-continuation", "tools/call");
    assert(tools == 4 && tool_calls == 1);
    audio("mcp-continuation");
    session_snapshot(&state);
    assert(!state.turn_done && ringbuf_avail() == 8);
    event("mcp-continuation", TAI_EVT_CHAT_BREAK);
    session_snapshot(&state);
    assert(state.break_seq == 1 && !state.got_audio && !ringbuf_avail());
    audio("mcp-continuation"); /* Server break didn't retire this valid event. */
    session_snapshot(&state);
    assert(state.got_audio && ringbuf_avail() == 4);
    /* The callback cleared old audio before this fresh frame arrived. The audio
     * loop must see its break epoch together with the frame, not clear it again. */
    unsigned audio_epoch = 0;
    assert(session_pop_audio(NULL, 0, &audio_epoch) == 0);
    assert(audio_epoch == 1 && ringbuf_avail() == 4);
    assert(session_pop_audio(NULL, 4, &audio_epoch) == 0);
    assert(ringbuf_avail() == 4);
    int16_t decoded[4];
    assert(session_pop_audio(decoded, 4, NULL) == 0);
    assert(ringbuf_avail() == 4);
    assert(session_pop_audio(decoded, 4, &audio_epoch) == 4);
    assert(audio_epoch == 1 && !ringbuf_avail());
    assert(decoded[0] == 100 && decoded[1] == -100 && decoded[2] == 200 && decoded[3] == -200);
    assert(session_tx_error() == 0);

    /* Admission happened before reset, but parsing completes afterwards. No old
     * UI text may follow the serialized new-turn clear. */
    pthread_mutex_lock(&parse_mutex);
    pause_ui_parse = 1;
    pthread_mutex_unlock(&parse_mutex);
    assert(!pthread_create(&producer, NULL, late_nlg, NULL));
    wait_parse();
    assert(session_post_start() == 0);
    wait_starts(3);
    assert(ui_clears == 3 && !displayed[0]);
    pthread_mutex_lock(&parse_mutex);
    parse_released = 1;
    pthread_cond_broadcast(&parse_cond);
    pthread_mutex_unlock(&parse_mutex);
    assert(!pthread_join(producer, NULL));
    assert(!displayed[0]);
    asr("after-ui-reset");
    nlg("after-ui-reset", "fresh answer");
    assert(!strcmp(displayed, "fresh answer"));

    /* A photo waits behind START..END; canceling that generation while it
     * waits must not relabel it or upload into the next active audio event. */
    photo_job_t stale_photo = {.generation = current_generation(), .result = -999};
    pthread_t photographer;
    assert(!pthread_create(&photographer, NULL, send_photo, &stale_photo));
    wait_event_waiter();
    pthread_mutex_lock(&tx_mutex);
    assert(!images);
    pthread_mutex_unlock(&tx_mutex);
    assert(session_post_interrupt() == 0);
    assert(session_post_start() == 0);
    wait_starts(4);
    assert(!pthread_join(photographer, NULL));
    assert(stale_photo.result == SESSION_PHOTO_CANCELED && !images);

    asr("photo-live");
    assert(session_post_end() == 0);
    wait_ends(4); /* The canceled, still-open third request also got an END. */
    photo_job_t live_photo = {.generation = current_generation(), .result = -999};
    pthread_mutex_lock(&tx_mutex);
    block_image = 1;
    pthread_mutex_unlock(&tx_mutex);
    assert(!pthread_create(&photographer, NULL, send_photo, &live_photo));
    wait_for_image();
    assert(session_post_interrupt() == 0);
    assert(session_post_start() == 0);
    wait_event_waiter();
    pthread_mutex_lock(&tx_mutex);
    assert(starts == 4 && images == 1); /* No START inside the image event. */
    release_image = 1;
    pthread_cond_broadcast(&tx_cond);
    pthread_mutex_unlock(&tx_mutex);
    assert(!pthread_join(photographer, NULL));
    assert(live_photo.result == 0); /* Already in-flight photo may finish. */
    wait_starts(5);
    asr("final");
    assert(session_post_end() == 0);
    wait_ends(5);

    assert(ringbuf_init(2, 1) == 0);
    audio("final");
    session_snapshot(&state);
    assert(state.lost && !state.got_audio && !ringbuf_avail());
    session_duplex_disable();
    assert(starts == 5 && breaks == 3 && !chunks && ends == 5 && images == 1 && tools == 4 && tool_calls == 1);

    /* Worst-case producer schedule: a 1s pre-roll burst plus 2s of continuing
     * capture while one send is blocked. 150 x20ms frames must drain intact,
     * including END, once the transport becomes writable. No sleeping needed. */
    memset(&s_st, 0, sizeof(s_st));
    assert(session_duplex_enable() == 0);
    assert(session_post_start() == 0);
    wait_starts(6);
    pthread_mutex_lock(&tx_mutex);
    stall_audio = 1;
    pthread_mutex_unlock(&tx_mutex);
    int16_t pcm[320] = {0};
    assert(session_post_chunk(pcm, 320) == 0);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 3;
    pthread_mutex_lock(&tx_mutex);
    while (!audio_entered) assert(!pthread_cond_timedwait(&tx_cond, &tx_mutex, &until));
    pthread_mutex_unlock(&tx_mutex);
    for (int i = 1; i < 150; i++) {
        pcm[0] = (int16_t)i;
        assert(session_post_chunk(pcm, 320) == 0);
    }
    assert(session_post_end() == 0 && session_tx_error() == 0);
    pthread_mutex_lock(&tx_mutex);
    release_audio = 1;
    pthread_cond_broadcast(&tx_cond);
    pthread_mutex_unlock(&tx_mutex);
    wait_ends(6);
    session_duplex_disable();
    assert(sequence_used == 150 && chunks == 150);
    for (int i = 0; i < 150; i++) assert(sent_sequence[i] == i);
    session_close();
    assert(!s_ctx && sdk_disconnects == 1 && sdk_deinits == 1);
    session_close();
    assert(sdk_disconnects == 1 && sdk_deinits == 1);
    ringbuf_free();
    puts("session callbacks: stale-ID race, MCP continuation and server-break checks passed");
    return 0;
}
