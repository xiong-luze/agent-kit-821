/* Real SDK dispatcher, no network or credentials. Reproduce the board order:
 * START(new), CHAT_BREAK(old), then ASR/TTS without explicit event-id. */
#include "tai_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

extern const pal_t *tai_pal_posix(void);
static char text_id[64], audio_id[64], event_id[64];

static void on_text(tai_ctx_t *ctx, const tai_text_msg_t *msg, void *data)
{
    (void)ctx; (void)data;
    snprintf(text_id, sizeof(text_id), "%s", msg->event_id);
}
static void on_audio(tai_ctx_t *ctx, const tai_audio_msg_t *msg, void *data)
{
    (void)ctx; (void)data;
    snprintf(audio_id, sizeof(audio_id), "%s", msg->event_id);
}
static void on_event(tai_ctx_t *ctx, const tai_event_msg_t *msg, void *data)
{
    (void)ctx; (void)data;
    snprintf(event_id, sizeof(event_id), "%s", msg->event_id);
}

static void event(tai_ctx_t *ctx, uint16_t type, const char *id)
{
    uint8_t payload[16];
    int len = tai_pack_event(TAI_VER_21, type, NULL, 0, payload, sizeof(payload));
    assert(len > 0);
    tai_attr_t attr = tai_attr_strv(TAI_ATTR_EVENT_ID, id ? id : "");
    assert(tai_proto_dispatch(ctx, TAI_PKT_EVENT, id ? &attr : NULL, id ? 1 : 0,
                              payload, (size_t)len) == TAI_OK);
}

static void media_without_id(tai_ctx_t *ctx, const char *expected)
{
    uint8_t text[32], audio[16];
    int len = tai_pack_text_hdr(TAI_VER_21, TAI_DATA_ID_TEXT_DOWN, TAI_STREAM_ONE_SHOT,
                                1, text, sizeof(text));
    assert(len > 0);
    text[len++] = 'x';
    assert(tai_proto_dispatch(ctx, TAI_PKT_TEXT, NULL, 0, text, (size_t)len) == TAI_OK);
    assert(!strcmp(text_id, expected));
    len = tai_pack_media_hdr(TAI_VER_21, TAI_DATA_ID_AUDIO_DOWN, TAI_STREAM_ONE_SHOT,
                            0, audio, sizeof(audio));
    assert(len > 0);
    audio[len++] = 0;
    audio[len++] = 0;
    assert(tai_proto_dispatch(ctx, TAI_PKT_AUDIO, NULL, 0, audio, (size_t)len) == TAI_OK);
    assert(!strcmp(audio_id, expected));
}

int main(void)
{
    struct tai_ctx ctx = {0};
    ctx.proto_ver = TAI_VER_21;
    ctx.pal = tai_pal_posix();
    ctx.on_text = on_text;
    ctx.on_audio = on_audio;
    ctx.on_event = on_event;
    event(&ctx, TAI_EVT_START, "old");
    event(&ctx, TAI_EVT_START, "new");
    strcpy(ctx.event_id, "new");
    ctx.event_open = 1;
    event(&ctx, TAI_EVT_CHAT_BREAK, "old");
    assert(!strcmp(event_id, "old")); /* Callback still identifies the old event. */
    media_without_id(&ctx, "new");
    event(&ctx, TAI_EVT_END, "old");
    assert(!strcmp(event_id, "old"));
    assert(ctx.event_open == 1);
    media_without_id(&ctx, "new");
    event(&ctx, TAI_EVT_END, "new");
    assert(!strcmp(event_id, "new"));
    assert(ctx.event_open == 0);
    media_without_id(&ctx, "");
    event(&ctx, TAI_EVT_START, "next");
    event(&ctx, TAI_EVT_CHAT_BREAK, NULL);
    assert(!strcmp(event_id, "next"));
    media_without_id(&ctx, "next");
    event(&ctx, TAI_EVT_END, NULL);
    assert(!strcmp(event_id, "next"));
    media_without_id(&ctx, "");
    puts("SDK late-control inheritance: passed");
    return 0;
}
