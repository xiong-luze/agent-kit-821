/* session.c —— 见 session.h */
#include "session.h"
#include "mcp.h"
#include "audio_uplink.h"
#include "turn_guard.h"
#include "../core/jsonx.h"
#include "../core/ringbuf.h"
#include "../core/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "iot_client.h"

extern const pal_t *tai_pal_posix(void);

static session_state_t s_st;
static session_ui_t    s_ui;
static tai_ctx_t      *s_ctx = NULL;
static iot_client_t   *s_iot = NULL;
static void           *s_ctx_buf = NULL;
static pthread_mutex_t s_state_mutex = PTHREAD_MUTEX_INITIALIZER;
static audio_uplink_t *s_uplink;
static int s_duplex;
static turn_guard_t s_guard;
static int s_voice_trace;
static uint64_t s_trace_generation;
static unsigned s_trace_seen[4][2], s_trace_audio;
/* Logical event ownership spans worker START..END without leaving a mutex
 * locked between callbacks. This also makes queue shutdown/cancel safe. */
static pthread_mutex_t s_event_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_event_cond = PTHREAD_COND_INITIALIZER;
static int s_audio_event_open, s_event_busy, s_event_stopping, s_event_failed;
static unsigned s_event_waiters, s_photo_calls;

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Optional timing only: forward the original PAL arguments and results intact.
 * No payload, host, handle, credential or transcript is logged here. */
static const pal_t *s_trace_pal_base;
static pal_t s_trace_pal;

static int trace_tcp_send(void *handle, const uint8_t *buffer, size_t size, uint32_t timeout)
{
    int64_t before = now_ms();
    int rc = s_trace_pal_base->tcp_send(handle, buffer, size, timeout);
    int64_t elapsed = now_ms() - before;
    if (elapsed >= 40 || (rc < 0 && rc != PAL_ERR_AGAIN))
        fprintf(stderr, "[网络诊断] phase=send t=%lld bytes=%zu rc=%d elapsed_ms=%lld\n",
                (long long)before, size, rc, (long long)elapsed);
    return rc;
}

static int trace_tcp_poll(void *handle, int events, uint32_t timeout)
{
    int64_t before = now_ms();
    int rc = s_trace_pal_base->tcp_poll(handle, events, timeout);
    int64_t elapsed = now_ms() - before;
    if (rc < 0 || (events & 2 && elapsed >= 40))
        fprintf(stderr, "[网络诊断] phase=poll t=%lld events=%d rc=%d elapsed_ms=%lld\n",
                (long long)before, events, rc, (long long)elapsed);
    return rc;
}

static void trace_mutex_lock(void *mutex)
{
    int64_t before = now_ms();
    s_trace_pal_base->mutex_lock(mutex);
    int64_t elapsed = now_ms() - before;
    if (elapsed >= 40)
        fprintf(stderr, "[网络诊断] phase=lock t=%lld elapsed_ms=%lld\n",
                (long long)before, (long long)elapsed);
}

static const pal_t *session_pal(void)
{
    const pal_t *base = tai_pal_posix();
    if (!s_voice_trace || !base) return base;
    s_trace_pal_base = base;
    s_trace_pal = *base;
    s_trace_pal.tcp_send = trace_tcp_send;
    s_trace_pal.tcp_poll = trace_tcp_poll;
    s_trace_pal.mutex_lock = trace_mutex_lock;
    return &s_trace_pal;
}

const session_state_t *session_state(void) { return &s_st; }
tai_ctx_t             *session_ctx(void)   { return s_ctx; }

void session_snapshot(session_state_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&s_state_mutex);
    *out = s_st;
    pthread_mutex_unlock(&s_state_mutex);
}

size_t session_pop_audio(int16_t *out, size_t samples, unsigned *break_seq)
{
    if (!break_seq) return 0;
    pthread_mutex_lock(&s_state_mutex);
    *break_seq = s_st.break_seq;
    size_t count = out && samples ? ringbuf_pop_frame(out, samples) : 0;
    pthread_mutex_unlock(&s_state_mutex);
    return count;
}

/* Called with s_state_mutex held. Audio admission and ring insertion/clear use
 * the same lock order (state -> ring), closing the cancel/late-callback race. */
static int admit(const char *event_id, turn_guard_kind_t kind)
{
    if (!s_duplex) return 1;
    int accepted = turn_guard_accept(&s_guard, event_id, kind);
    if (s_voice_trace) {
        if (s_trace_generation != s_guard.generation) {
            s_trace_generation = s_guard.generation;
            memset(s_trace_seen, 0, sizeof(s_trace_seen));
            s_trace_audio = 0;
        }
        if (!s_trace_seen[kind][!!accepted]++)
            fprintf(stderr, "[会话诊断] rx_kind=%d admitted=%d gen=%llu has_id=%d ready=%d sent=%d\n",
                    kind, accepted, (unsigned long long)s_guard.generation,
                    event_id && *event_id, s_guard.ready, s_guard.request_sent);
    }
    return accepted;
}

static void dispatch_ui(uint64_t generation, void (*callback)(const char *), const char *text)
{
    if (!callback) return;
    pthread_mutex_lock(&s_state_mutex);
    if (!s_duplex || generation == s_guard.generation) callback(text);
    pthread_mutex_unlock(&s_state_mutex);
}

int session_generation_is_current(uint64_t generation)
{
    pthread_mutex_lock(&s_state_mutex);
    int valid = !s_st.lost && (s_duplex ? generation != 0 && generation == s_guard.generation &&
                              s_guard.ready : generation == 0);
    pthread_mutex_unlock(&s_state_mutex);
    return valid;
}

void session_status_for_generation(uint64_t generation, const char *status)
{
    if (!status) return;
    pthread_mutex_lock(&s_state_mutex);
    if (!s_st.lost && (s_duplex ? generation != 0 && generation == s_guard.generation : generation == 0))
        if (s_ui.on_status) s_ui.on_status(status);
    pthread_mutex_unlock(&s_state_mutex);
}

/* Event mutex held. Waiters are counted so teardown can observe their exit. */
static int wait_event_slot(int exclude_audio, uint64_t generation)
{
    if (exclude_audio && !session_generation_is_current(generation)) return SESSION_PHOTO_CANCELED;
    if ((s_event_busy || (exclude_audio && s_audio_event_open)) &&
        !s_event_stopping && !s_event_failed) {
        s_event_waiters++;
        pthread_cond_broadcast(&s_event_cond);
        while ((s_event_busy || (exclude_audio && s_audio_event_open)) &&
               !s_event_stopping && !s_event_failed &&
               (!exclude_audio || session_generation_is_current(generation)))
            pthread_cond_wait(&s_event_cond, &s_event_mutex);
        s_event_waiters--;
        pthread_cond_broadcast(&s_event_cond);
    }
    if (exclude_audio && !session_generation_is_current(generation)) return SESSION_PHOTO_CANCELED;
    return s_event_failed ? s_event_failed : s_event_stopping ? -EPIPE : 0;
}

int session_send_photo_for_generation(uint64_t generation, const char *hint,
                                      const uint8_t *jpeg, size_t bytes,
                                      uint16_t width, uint16_t height)
{
    if (!hint || !jpeg || !bytes || !width || !height) return -EINVAL;
    if (!session_generation_is_current(generation)) return SESSION_PHOTO_CANCELED;
    pthread_mutex_lock(&s_event_mutex);
    s_photo_calls++;
    pthread_cond_broadcast(&s_event_cond);
    int error = wait_event_slot(1, generation);
    if (error) {
        int canceled = s_event_stopping;
        s_photo_calls--;
        pthread_cond_broadcast(&s_event_cond);
        pthread_mutex_unlock(&s_event_mutex);
        return canceled ? SESSION_PHOTO_CANCELED : error;
    }
    s_event_busy = 1;
    pthread_mutex_unlock(&s_event_mutex);

    int rc = SESSION_PHOTO_CANCELED;
    if (session_generation_is_current(generation)) {
        if (!s_ctx) rc = -EINVAL;
        else rc = tai_send_image_with_text(s_ctx, hint, strlen(hint), jpeg, bytes,
                                           TAI_IMG_JPEG, width, height);
    }
    pthread_mutex_lock(&s_event_mutex);
    s_event_busy = 0;
    s_photo_calls--;
    if (rc < 0) s_event_failed = rc;
    pthread_cond_broadcast(&s_event_cond);
    pthread_mutex_unlock(&s_event_mutex);
    if (rc < 0) {
        pthread_mutex_lock(&s_state_mutex);
        if (s_duplex) s_st.lost = 1;
        pthread_mutex_unlock(&s_state_mutex);
    }
    return rc;
}

/* -- session token 解析（本文件私有） ------------------------------------
 * token 是 base64 包着的 JSON，里面有 TAI 的 host/port/SNI/derived_client_id
 * 和 agentToken。逻辑原样取自 SDK 的 posix 例程。 */
typedef struct {
    char     host[256];
    char     tls_sni[256];
    char     derived_client_id[256];
    char     agent_token[256];
    uint16_t port;
    long     biz_code;
    long     biz_tag;
} tai_conn_params_t;


static int parse_token(const char *raw_token, tai_conn_params_t *p)
{
    memset(p, 0, sizeof(*p));
    char *json = NULL;
    {
        size_t dl = 0;
        char  *decoded = b64_decode(raw_token, &dl);
        if (decoded && dl > 0 && decoded[0] == '{')
            json = decoded;
        else {
            free(decoded);
            json = strdup(raw_token);
        }
    }
    if (!json) return -1;

    char *conn = json_get_object(json, "connect_conf");
    if (!conn) {
        fprintf(stderr, "[parse_token] 'connect_conf' not found\n");
        free(json);
        return -1;
    }

    json_array_first_string(conn, "hosts",   p->host,    sizeof(p->host));
    if (json_array_first_string(conn, "domains", p->tls_sni, sizeof(p->tls_sni)) != 0)
        strncpy(p->tls_sni, p->host, sizeof(p->tls_sni) - 1);

    long port = 0;
    if (json_get_long(conn, "ecc_tls_port", &port) != 0)
        json_get_long(conn, "tcpport", &port);
    p->port = (port > 0) ? (uint16_t)port : 443;

    json_get_string(conn, "derived_client_id",
                    p->derived_client_id, sizeof(p->derived_client_id));
    free(conn);

    char *sess = json_get_object(json, "session_conf");
    if (sess) {
        json_get_string(sess, "agentToken",
                        p->agent_token, sizeof(p->agent_token));
        char *biz = json_get_object(sess, "bizConfig");
        if (biz) {
            json_get_long(biz, "bizCode", &p->biz_code);
            json_get_long(biz, "bizTag",  &p->biz_tag);
            free(biz);
        }
        free(sess);
    }

    free(json);
    if (p->host[0] == '\0') {
        fprintf(stderr, "[parse_token] Could not extract host\n");
        return -1;
    }
    return 0;
}

static void on_text(tai_ctx_t *ctx, const tai_text_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    if (!msg->text || msg->len == 0) return;

    char *tmp = (char *)malloc(msg->len + 1);
    if (!tmp) return;
    memcpy(tmp, msg->text, msg->len);
    tmp[msg->len] = '\0';

    char biz[16] = {0};
    json_get_string(tmp, "bizType", biz, sizeof(biz));
    char *data = json_get_object(tmp, "data");
    pthread_mutex_lock(&s_state_mutex);
    int accepted = admit(msg->event_id, !strcmp(biz, "ASR") ? TURN_GUARD_ASR :
                         msg->stream_flag == TAI_STREAM_START ? TURN_GUARD_START : TURN_GUARD_MEDIA);
    uint64_t generation = s_guard.generation;
    pthread_mutex_unlock(&s_state_mutex);
    if (!accepted) { free(data); free(tmp); return; }
    if (data) {
        if (strcmp(biz, "ASR") == 0) {
            char asr[512] = {0};
            if (json_get_string_esc(data, "text", asr, sizeof(asr)) == 0 && asr[0]) {
                json_unescape_u_inplace(asr);      /* 云端双重编码的残留 */
                printf("\r[你说] %s          ", asr); fflush(stdout);
                dispatch_ui(generation, s_ui.on_user_text, asr);
            }
            long eof = 0;
            if (json_get_long(tmp, "eof", &eof) == 0 && eof == 1) {
                pthread_mutex_lock(&s_state_mutex);
                if (!s_duplex || generation == s_guard.generation) s_st.asr_final = 1;
                pthread_mutex_unlock(&s_state_mutex);
            }
        } else if (strcmp(biz, "SKILL") == 0) {
            char *sc = json_get_object(data, "skillContent");
            if (sc) {
                /* emotion:["HAPPY"] → 可映射 /usr/share/xstar/assets/eyes/happy/ */
                const char *p = strstr(sc, "\"emotion\"");
                if (p && (p = strchr(p, '[')) && (p = strchr(p, '"'))) {
                    char emo[32] = {0};
                    const char *e = strchr(++p, '"');
                    if (e && (size_t)(e - p) < sizeof(emo)) {
                        memcpy(emo, p, (size_t)(e - p));
                        printf("\n[情绪] %s\n", emo);
                        dispatch_ui(generation, s_ui.on_emotion, emo);
                    }
                }
                free(sc);
            }
        } else {
            char content[1024] = {0};
            /* 必须用转义感知版：云端的 content 带 \u0020 这类转义，朴素提取器
             * 会把它原样抄成 "\u0020" 6 个字符显示出来（屏幕上的乱码）。 */
            if (json_get_string_esc(data, "content", content, sizeof(content)) == 0
                    && content[0]) {
                json_unescape_u_inplace(content);  /* 再解一层字面量 \uXXXX */
                char clean[1024];
                uint32_t cp = 0;
                text_strip_emoji(content, clean, sizeof(clean), &cp);
                printf("%s", clean); fflush(stdout);
                dispatch_ui(generation, s_ui.on_ai_text, clean);
            }
        }
        free(data);
    }
    free(tmp);
}

/* TTS 音频：只入 ring，不直接写 aplay —— 播放节奏由主循环（采集时钟）控制 */
static void on_audio(tai_ctx_t *ctx, const tai_audio_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    if (!msg->data || msg->len == 0) return;
    if (msg->codec != TAI_AUDIO_PCM || msg->len % sizeof(int16_t) ||
        (msg->sample_rate && msg->sample_rate != 16000)) {
        fprintf(stderr, "[音频] 无效 PCM 参数 codec=%d rate=%u bytes=%zu\n",
                (int)msg->codec, (unsigned)msg->sample_rate, msg->len);
        return;
    }
    pthread_mutex_lock(&s_state_mutex);
    int accepted_audio = admit(msg->event_id, msg->stream_flag == TAI_STREAM_START ||
               msg->stream_flag == TAI_STREAM_ONE_SHOT ? TURN_GUARD_START : TURN_GUARD_MEDIA);
    if (s_voice_trace && !s_trace_audio++)
        fprintf(stderr, "[会话诊断] first_audio bytes=%zu flag=%d admitted=%d\n",
                msg->len, msg->stream_flag, accepted_audio);
    if (!accepted_audio) {
        pthread_mutex_unlock(&s_state_mutex);
        return;
    }
    s_st.last_audio_ms = now_ms();
    if (msg->stream_flag == TAI_STREAM_START) s_st.turn_done = 0;
    s_st.got_audio = 1;
    /* Packet payload pointers need not be int16-aligned on riscv32. Decode the
     * negotiated S16_LE explicitly rather than casting borrowed wire bytes. */
    int16_t aligned[AUDIO_UPLINK_MAX_SAMPLES];
    size_t total = msg->len / sizeof(int16_t);
    for (size_t offset = 0; offset < total;) {
        size_t count = total - offset;
        if (count > AUDIO_UPLINK_MAX_SAMPLES) count = AUDIO_UPLINK_MAX_SAMPLES;
        for (size_t i = 0; i < count; i++) {
            size_t byte = (offset + i) * 2;
            aligned[i] = (int16_t)((uint16_t)msg->data[byte] |
                                   (uint16_t)msg->data[byte + 1] << 8);
        }
        size_t accepted = ringbuf_push(aligned, count);
        if (accepted != count) {
            fprintf(stderr, "[音频] 播放队列溢出：接收 %zu/%zu 样本%s\n",
                    accepted, count, s_duplex ? "，结束本次连接" : "");
            if (s_duplex) {
                s_st.lost = 1;
                s_st.got_audio = 0;
                ringbuf_clear();
            }
            break;
        }
        offset += count;
    }
    pthread_mutex_unlock(&s_state_mutex);
}

static void on_event(tai_ctx_t *ctx, const tai_event_msg_t *msg, void *ud)
{
    (void)ud;
    char *mcp_json = NULL;
    int connection_mcp = 0;
    if (msg->event_type == TAI_EVT_MCP_CMD && msg->data && msg->len) {
        mcp_json = malloc(msg->len + 1);
        if (!mcp_json) return;
        memcpy(mcp_json, msg->data, msg->len);
        mcp_json[msg->len] = '\0';
        char method[64] = {0};
        if (json_get_string_esc(mcp_json, "method", method, sizeof(method)) == 0)
            connection_mcp = !strcmp(method, "initialize") || !strcmp(method, "tools/list") ||
                !strncmp(method, "notifications/", sizeof("notifications/") - 1);
    }
    pthread_mutex_lock(&s_state_mutex);
    turn_guard_kind_t kind = msg->event_type == TAI_EVT_START ||
        msg->event_type == TAI_EVT_MCP_CMD ? TURN_GUARD_START : TURN_GUARD_END;
    int accepted = connection_mcp || admit(msg->event_id, kind);
    if (accepted) {
        if (msg->event_type == TAI_EVT_START) s_st.turn_done = 0;
        else if (msg->event_type == TAI_EVT_END) s_st.turn_done = 1;
        else if (msg->event_type == TAI_EVT_CHAT_BREAK && s_duplex) {
            s_st.break_seq++;
            s_st.got_audio = 0;
            ringbuf_clear();
        }
    }
    uint64_t mcp_generation = s_duplex && !connection_mcp ? s_guard.generation : 0;
    pthread_mutex_unlock(&s_state_mutex);
    if (accepted && mcp_json) mcp_handle_generation(ctx, mcp_json, mcp_generation);
    free(mcp_json);
}

static void on_disconnect(tai_ctx_t *ctx, const tai_disconnect_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    fprintf(stderr, "\n[连接] 断开: reason=%u code=%u\n",
            msg->reason, (unsigned)msg->close_code);
    pthread_mutex_lock(&s_state_mutex);
    s_st.lost = 1;
    pthread_mutex_unlock(&s_state_mutex);
}


/* -- 连云 ---------------------------------------------------------------- */

static const char SESSION_ATTRS[] =
    "{\"deviceMcp\":{\"supportCustomMCP\":true},"
    "\"tts.order.supports\":[{\"format\":\"pcm\","
    "\"sampleRate\":16000,\"bitDepth\":\"16\",\"channels\":1}]}";
static const char EVENT_ATTRS[] =
    "{\"sys.workflow\":\"asr-llm-tts\",\"asr.enableVad\":true,"
    "\"tts.alternate\":true,\"processing.interrupt\":true}";

int session_connect(const char *devid, const char *secret_key,
                    const char *local_key, const session_ui_t *ui)
{
    const char *trace = getenv("V821_VOICE_TRACE");
    s_voice_trace = trace && !strcmp(trace, "1");
    pthread_mutex_lock(&s_event_mutex);
    if (s_event_busy || s_photo_calls || s_event_waiters) {
        pthread_mutex_unlock(&s_event_mutex);
        return -EBUSY;
    }
    s_event_stopping = s_event_failed = s_audio_event_open = 0;
    pthread_mutex_unlock(&s_event_mutex);
    pthread_mutex_lock(&s_state_mutex);
    if (ui) s_ui = *ui;
    else memset(&s_ui, 0, sizeof(s_ui));
    memset(&s_st, 0, sizeof(s_st));
    turn_guard_init(&s_guard);
    pthread_mutex_unlock(&s_state_mutex);

    iot_client_config_t iot_cfg = {
        .region = AY, .env = PROD, .mqtt_disable_tls = false, .message_callback = NULL,
    };
    memcpy((char *)iot_cfg.devid,      devid,      strlen(devid));
    memcpy((char *)iot_cfg.secret_key, secret_key, strlen(secret_key));
    memcpy((char *)iot_cfg.local_key,  local_key,  strlen(local_key));
    s_iot = iot_client_init(&iot_cfg);
    if (!s_iot) { fprintf(stderr, "[连云] iot_client_init 失败\n"); return -1; }

    char *token = (char *)calloc(1, 4096);
    if (!token) return -1;
    if (iot_client_get_session_token(s_iot, NULL, token, 4096) != 0 || !token[0]) {
        fprintf(stderr, "[连云] 取 session token 失败\n"); free(token); return -1;
    }
    tai_conn_params_t cp;
    if (parse_token(token, &cp) != 0) { free(token); return -1; }
    free(token);
    if (cp.biz_code == 0) cp.biz_code = 65537;
    if (cp.biz_tag  == 0) cp.biz_tag  = 119;
    printf("[连云] TAI %s:%u (SNI %s)\n", cp.host, cp.port, cp.tls_sni);

    tai_config_t cfg = {
        .host = cp.host, .port = cp.port, .tls_sni = cp.tls_sni,
        .device_id = cp.derived_client_id, .local_key = local_key,
        .protocol_version = TAI_VER_21, .client_type = TAI_CLIENT_DEVICE,
        .sign_level = TAI_SIGN_HMAC_SHA256,
        .biz_code = (uint32_t)cp.biz_code, .biz_tag = (uint64_t)cp.biz_tag,
        .agent_token = cp.agent_token,
        .session_attrs_json = SESSION_ATTRS, .event_user_data_json = EVENT_ATTRS,
        .pal = session_pal(),
        .on_text = on_text, .on_audio = on_audio,
        .on_event = on_event, .on_disconnect = on_disconnect,
        .user_data = NULL,
    };
    s_ctx_buf = malloc(tai_ctx_size());
    if (!s_ctx_buf) return -1;
    s_ctx = tai_ctx_init(s_ctx_buf, &cfg);
    if (!s_ctx) { fprintf(stderr, "[连云] tai_ctx_init 失败\n"); return -1; }
    if (tai_connect(s_ctx) != TAI_OK) {
        fprintf(stderr, "[连云] tai_connect 失败\n"); return -1;
    }
    printf("[连云] 已连接\n");
    return 0;
}

void session_close(void)
{
    session_duplex_disable();
    if (s_ctx) {
        /* ctx_deinit releases TLS/mutexes; it does NOT stop or join the SDK
         * receive worker. Join it before any context resources are freed. */
        tai_disconnect(s_ctx);
        tai_ctx_deinit(s_ctx);
        s_ctx = NULL;
    }
    free(s_ctx_buf); s_ctx_buf = NULL;
    if (s_iot) { iot_client_deinit(s_iot); s_iot = NULL; }
}

void session_turn_begin(void)
{
    pthread_mutex_lock(&s_state_mutex);
    s_st.turn_done = 0;
    s_st.got_audio = 0;
    s_st.asr_final = 0;
    s_st.last_audio_ms = 0;
    pthread_mutex_unlock(&s_state_mutex);
}

int session_audio_start(void)
{
    pthread_mutex_lock(&s_state_mutex);
    int duplex = s_duplex;
    pthread_mutex_unlock(&s_state_mutex);
    if (duplex) return -EPERM;
    return tai_send_audio_start(s_ctx, TAI_AUDIO_PCM, 1, 16, 16000) == TAI_OK ? 0 : -1;
}

int session_audio_chunk(const int16_t *pcm, int frames)
{
    pthread_mutex_lock(&s_state_mutex);
    int duplex = s_duplex;
    pthread_mutex_unlock(&s_state_mutex);
    if (duplex) return -EPERM;
    return tai_send_audio_chunk(s_ctx, (const uint8_t *)pcm,
                                (size_t)frames * sizeof(int16_t)) == TAI_OK ? 0 : -1;
}

int session_audio_end(void)
{
    pthread_mutex_lock(&s_state_mutex);
    int duplex = s_duplex;
    pthread_mutex_unlock(&s_state_mutex);
    if (duplex) return -EPERM;
    return tai_send_audio_end(s_ctx) == TAI_OK ? 0 : -1;
}

int session_send_text(const char *utf8)
{
    pthread_mutex_lock(&s_state_mutex);
    int duplex = s_duplex;
    pthread_mutex_unlock(&s_state_mutex);
    if (duplex) return -EPERM;
    if (!s_ctx || !utf8) return -1;
    return tai_send_text(s_ctx, utf8, strlen(utf8)) == TAI_OK ? 0 : -1;
}

static int duplex_send(void *context, const audio_uplink_item_t *item)
{
    tai_ctx_t *ctx = context;
    int rc = 0;
    int64_t trace_start = s_voice_trace ? now_ms() : 0;
    switch (item->kind) {
    case AUDIO_UPLINK_START:
        pthread_mutex_lock(&s_event_mutex);
        rc = wait_event_slot(0, 0);
        if (!rc && s_audio_event_open) rc = -EPROTO;
        if (!rc) s_audio_event_open = 1;
        pthread_mutex_unlock(&s_event_mutex);
        if (rc) return rc;
        /* Arm only when the worker actually starts this request, not when the
         * audio thread queues it. A receive callback may run before send returns. */
        pthread_mutex_lock(&s_state_mutex);
        turn_guard_request_sent(&s_guard, item->generation);
        pthread_mutex_unlock(&s_state_mutex);
        rc = tai_send_audio_start(ctx, TAI_AUDIO_PCM, 1, 16, 16000);
        break;
    case AUDIO_UPLINK_CHUNK:
        pthread_mutex_lock(&s_event_mutex);
        rc = s_event_failed ? s_event_failed :
            !s_audio_event_open || s_event_busy || s_event_stopping ? -EPROTO : 0;
        pthread_mutex_unlock(&s_event_mutex);
        if (rc) return rc;
        rc = tai_send_audio_chunk(ctx, (const uint8_t *)item->pcm,
                                   item->samples * sizeof(int16_t));
        break;
    case AUDIO_UPLINK_END:
        pthread_mutex_lock(&s_event_mutex);
        rc = s_event_failed ? s_event_failed :
            !s_audio_event_open || s_event_busy || s_event_stopping ? -EPROTO : 0;
        pthread_mutex_unlock(&s_event_mutex);
        if (rc) return rc;
        rc = tai_send_audio_end(ctx);
        break;
    case AUDIO_UPLINK_INTERRUPT:
        pthread_mutex_lock(&s_event_mutex);
        rc = wait_event_slot(0, 0);
        int open = s_audio_event_open;
        if (!rc) s_event_busy = 1;
        pthread_mutex_unlock(&s_event_mutex);
        if (rc) return rc;
        rc = tai_chat_break(ctx);
        if (!rc && open) rc = tai_send_audio_end(ctx);
        break;
    default:
        return -EINVAL;
    }
    pthread_mutex_lock(&s_event_mutex);
    if (item->kind == AUDIO_UPLINK_END || item->kind == AUDIO_UPLINK_INTERRUPT || rc)
        s_audio_event_open = 0;
    if (item->kind == AUDIO_UPLINK_INTERRUPT) s_event_busy = 0;
    if (rc < 0) s_event_failed = rc;
    pthread_cond_broadcast(&s_event_cond);
    pthread_mutex_unlock(&s_event_mutex);
    if (s_voice_trace && (item->kind != AUDIO_UPLINK_CHUNK || rc || now_ms() - trace_start >= 40))
        fprintf(stderr, "[会话诊断] tx_kind=%d gen=%llu rc=%d elapsed_ms=%lld\n",
                item->kind, (unsigned long long)item->generation, rc,
                (long long)(now_ms() - trace_start));
    return rc;
}

int session_duplex_enable(void)
{
    if (!s_ctx || s_uplink) return -EINVAL;
    pthread_mutex_lock(&s_event_mutex);
    while (s_event_busy || s_photo_calls) pthread_cond_wait(&s_event_cond, &s_event_mutex);
    s_event_stopping = s_event_failed = s_audio_event_open = 0;
    pthread_mutex_unlock(&s_event_mutex);
    /* 2s bounded transport jitter + up to 1s pre-roll at 20ms/frame,
     * with ten control/headroom slots. Board trace recovered after 1467ms;
     * the former 64 slots overflowed at 1317ms. ~105KB total on rv32. */
    audio_uplink_t *queue = audio_uplink_create(160, duplex_send, s_ctx);
    if (!queue) return -ENOMEM;
    pthread_mutex_lock(&s_state_mutex);
    turn_guard_init(&s_guard);
    s_duplex = 1;
    s_uplink = queue;
    pthread_mutex_unlock(&s_state_mutex);
    return 0;
}

void session_duplex_disable(void)
{
    /* Posting APIs are owned by the application thread, which must stop posting
     * before disable. The worker never accesses s_uplink itself. */
    audio_uplink_t *queue = s_uplink;
    pthread_mutex_lock(&s_event_mutex);
    s_event_stopping = 1;
    pthread_cond_broadcast(&s_event_cond);
    pthread_mutex_unlock(&s_event_mutex);
    audio_uplink_destroy(queue);
    pthread_mutex_lock(&s_event_mutex);
    while (s_event_busy || s_photo_calls || s_event_waiters)
        pthread_cond_wait(&s_event_cond, &s_event_mutex);
    s_audio_event_open = 0;
    pthread_mutex_unlock(&s_event_mutex);
    pthread_mutex_lock(&s_state_mutex);
    s_uplink = NULL;
    s_duplex = 0;
    pthread_mutex_unlock(&s_state_mutex);
}

static uint64_t reset_duplex_turn(int interrupt)
{
    pthread_mutex_lock(&s_state_mutex);
    uint64_t generation = interrupt ? turn_guard_interrupt(&s_guard) : turn_guard_begin(&s_guard);
    s_st.turn_done = 0;
    s_st.got_audio = 0;
    s_st.asr_final = 0;
    s_st.last_audio_ms = 0;
    ringbuf_clear();
    if (!interrupt && s_ui.on_turn_begin) s_ui.on_turn_begin();
    pthread_mutex_unlock(&s_state_mutex);
    /* Wake a photo waiting behind the previous audio event so it can reject
     * its now-stale generation, even if the next audio event starts at once. */
    pthread_mutex_lock(&s_event_mutex);
    pthread_cond_broadcast(&s_event_cond);
    pthread_mutex_unlock(&s_event_mutex);
    return generation;
}

int session_post_start(void)
{
    if (!s_uplink) return -EINVAL;
    uint64_t generation = reset_duplex_turn(0);
    return audio_uplink_post(s_uplink, AUDIO_UPLINK_START, NULL, 0, generation);
}

int session_post_chunk(const int16_t *pcm, int frames)
{
    if (!s_uplink || frames <= 0 || frames > AUDIO_UPLINK_MAX_SAMPLES) return -EINVAL;
    pthread_mutex_lock(&s_state_mutex);
    uint64_t generation = s_guard.generation;
    pthread_mutex_unlock(&s_state_mutex);
    return audio_uplink_post(s_uplink, AUDIO_UPLINK_CHUNK, pcm, (size_t)frames, generation);
}

int session_post_end(void)
{
    if (!s_uplink) return -EINVAL;
    pthread_mutex_lock(&s_state_mutex);
    uint64_t generation = s_guard.generation;
    pthread_mutex_unlock(&s_state_mutex);
    return audio_uplink_post(s_uplink, AUDIO_UPLINK_END, NULL, 0, generation);
}

int session_post_interrupt(void)
{
    if (!s_uplink) return -EINVAL;
    uint64_t generation = reset_duplex_turn(1);
    return audio_uplink_interrupt(s_uplink, generation);
}

int session_tx_error(void)
{
    if (!s_uplink) return -EINVAL;
    int error = audio_uplink_error(s_uplink);
    pthread_mutex_lock(&s_state_mutex);
    if (!error) error = turn_guard_error(&s_guard);
    pthread_mutex_unlock(&s_state_mutex);
    return error;
}
