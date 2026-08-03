/* session.c —— 见 session.h */
#include "session.h"
#include "mcp.h"
#include "../core/jsonx.h"
#include "../core/ringbuf.h"
#include "../core/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "iot_client.h"

extern const pal_t *tai_pal_posix(void);

static session_state_t s_st;
static session_ui_t    s_ui;
static tai_ctx_t      *s_ctx = NULL;
static iot_client_t   *s_iot = NULL;
static void           *s_ctx_buf = NULL;

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

const session_state_t *session_state(void) { return &s_st; }
tai_ctx_t             *session_ctx(void)   { return s_ctx; }

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
    if (data) {
        if (strcmp(biz, "ASR") == 0) {
            char asr[512] = {0};
            if (json_get_string_esc(data, "text", asr, sizeof(asr)) == 0 && asr[0]) {
                json_unescape_u_inplace(asr);      /* 云端双重编码的残留 */
                printf("\r[你说] %s          ", asr); fflush(stdout);
                if (s_ui.on_user_text) s_ui.on_user_text(asr);
            }
            long eof = 0;
            if (json_get_long(tmp, "eof", &eof) == 0 && eof == 1)
                s_st.asr_final = 1;          /* 云端认定这句说完了 */
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
                        if (s_ui.on_emotion) s_ui.on_emotion(emo);
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
                if (s_ui.on_ai_text) s_ui.on_ai_text(clean);
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
    if (msg->codec != TAI_AUDIO_PCM) {
        static int warned = 0;
        if (!warned++)
            fprintf(stderr, "\n[音频] 收到非 PCM(codec=%d)，已跳过\n", (int)msg->codec);
        return;
    }
    s_st.last_audio_ms = now_ms();
    /* 又来新音频流了：说明整体回答没结束（一次回答可能跨多个 TAI 事件），
     * 前一个事件的 END 置的 g_turn_done 不算数。 */
    if (msg->stream_flag == TAI_STREAM_START) s_st.turn_done = 0;

    if (!s_st.got_audio) { s_st.got_audio = 1; printf("\n[播放] ...\n"); }
    ringbuf_push((const int16_t *)msg->data, msg->len / sizeof(int16_t));
}

static void on_event(tai_ctx_t *ctx, const tai_event_msg_t *msg, void *ud)
{
    (void)ud;
    if (msg->event_type == TAI_EVT_START) {
        /* 新事件开始 —— 一次回答可能跨多个事件（MCP 拍照场景），
         * 上一个事件的 END 不能当成整体结束。 */
        s_st.turn_done = 0;
    } else if (msg->event_type == TAI_EVT_END) {
        s_st.turn_done = 1;
    } else if (msg->event_type == TAI_EVT_MCP_CMD) {
        /* 交给 mcp_tools：应答 initialize / tools/list / tools/call。
         * take_photo 只置标志，真正的拍照上传在 photo 线程里做，不阻塞这里。 */
        /* msg->data 带长度、不保证以 NUL 结尾，先拷成 C 字符串 */
        if (msg->data && msg->len) {
            char *j = (char *)malloc(msg->len + 1);
            if (j) {
                memcpy(j, msg->data, msg->len);
                j[msg->len] = '\0';
                mcp_handle(ctx, j);
                free(j);
            }
        }
    }
}

static void on_disconnect(tai_ctx_t *ctx, const tai_disconnect_msg_t *msg, void *ud)
{
    (void)ctx; (void)ud;
    fprintf(stderr, "\n[连接] 断开: reason=%u code=%u\n",
            msg->reason, (unsigned)msg->close_code);
    s_st.lost = 1;
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
    if (ui) s_ui = *ui;
    memset(&s_st, 0, sizeof(s_st));

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
        .pal = tai_pal_posix(),
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
    if (s_ctx) { tai_ctx_deinit(s_ctx); s_ctx = NULL; }
    free(s_ctx_buf); s_ctx_buf = NULL;
    if (s_iot) { iot_client_deinit(s_iot); s_iot = NULL; }
}

void session_turn_begin(void)
{
    s_st.turn_done = 0;
    s_st.got_audio = 0;
    s_st.asr_final = 0;
}

int session_audio_start(void)
{
    return tai_send_audio_start(s_ctx, TAI_AUDIO_PCM, 1, 16, 16000) == TAI_OK ? 0 : -1;
}

int session_audio_chunk(const int16_t *pcm, int frames)
{
    return tai_send_audio_chunk(s_ctx, (const uint8_t *)pcm,
                                (size_t)frames * sizeof(int16_t)) == TAI_OK ? 0 : -1;
}

int session_audio_end(void)
{
    return tai_send_audio_end(s_ctx) == TAI_OK ? 0 : -1;
}

int session_send_text(const char *utf8)
{
    if (!s_ctx || !utf8) return -1;
    return tai_send_text(s_ctx, utf8, strlen(utf8)) == TAI_OK ? 0 : -1;
}
