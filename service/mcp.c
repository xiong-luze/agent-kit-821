/*
 * mcp_tools.c —— 设备侧 MCP（Model Context Protocol）工具
 *
 * 云端会把 JSON-RPC 2.0 请求作为 TAI_EVT_MCP_CMD 事件推给设备，实际用到三个方法
 * （协议格式抄自 SDK 的 examples/posix/ai/rtc-tcp-client/mcp_demo.c）：
 *   initialize   -> serverInfo + capabilities
 *   tools/list   -> 设备暴露的工具目录（大模型据此决定要不要调）
 *   tools/call   -> 按名字分发
 *
 * 现在注册一个工具：take_photo。你说"看看有什么"之类，大模型判断需要看画面时
 * 就会调它，我们拍一张照上传。
 *
 * ★ 为什么图片不走 MCP 返回值：MCP 的工具结果只能是文本
 * （content:[{type:"text",...}]），装不了图片。所以拍照后走 TAI 的图像通道
 * tai_send_image_with_text() 上传，MCP 只回一句"已拍照"。
 *
 * ★ 为什么拍照放在独立线程：MCP 回调跑在 TAI 的接收线程上，而 V4L2 取帧 +
 * 软件 JPEG 编码在 rv32 上要几百毫秒。卡住接收线程会连带影响协议收发；放到
 * 音频主循环里更糟（今天已经反复吃过"主循环里做阻塞事"的亏）。所以 tools/call
 * 只置个标志立刻返回，真正的拍照上传由 photo 线程做。
 */
#include "mcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "tuya_ai.h"
#include "../core/jsonx.h"
#include "bsp/board.h"

#define MCP_PROTO   "2025-06-18"
#define MCP_NAME    "v821-chatbot"
#define MCP_VER     "0.1.0"

static tai_ctx_t      *g_ctx = NULL;
static volatile int    g_photo_req = 0;
static volatile int    g_quit = 0;
static pthread_t       g_thr;
static int             g_cam_ok = 0;
static char            g_photo_hint[256];   /* 上传图片时附带的文字 */
static void (*g_status_cb)(const char *) = NULL;

/* 注：这版 SDK 没有逐轮设置事件参数的接口（只有 tai_config_t 里静态的
 * session_attrs_json / event_user_data_json），所以不像 ESP 侧那样切
 * clm_intent=ai_image 的工作流。走 MCP 时本来就是大模型主动要求拍照的，
 * 图片进同一个 session 它就能看到，不需要额外切意图。 */

static void *photo_thread(void *arg)
{
    (void)arg;
    while (!g_quit) {
        if (!g_photo_req) { usleep(50 * 1000); continue; }
        g_photo_req = 0;

        if (!g_cam_ok) {
            fprintf(stderr, "[MCP] 相机不可用，跳过拍照\n");
            continue;
        }
        if (g_status_cb) g_status_cb("拍照中...");

        const uint8_t *jpg = NULL; size_t len = 0; int w = 0, h = 0;
        if (bsp_get()->camera_capture_jpeg(&jpg, &len, &w, &h) != 0) {
            if (g_status_cb) g_status_cb("拍照失败");
            continue;
        }

        /* 图片走图像通道上传。text 是给云端的提示语。 */
        const char *hint = g_photo_hint[0] ? g_photo_hint : "看看这是什么";
        int rc = tai_send_image_with_text(g_ctx, hint, strlen(hint),
                                          jpg, len, TAI_IMG_JPEG,
                                          (uint16_t)w, (uint16_t)h);
        if (rc != TAI_OK) {
            fprintf(stderr, "[MCP] 图片上传失败：%d\n", rc);
            if (g_status_cb) g_status_cb("图片上传失败");
        } else {
            printf("[MCP] 图片已上传（%u 字节，提示语「%s」）\n",
                   (unsigned)len, hint);
            if (g_status_cb) g_status_cb("图片已上传，等待理解...");
        }
    }
    return NULL;
}

int mcp_start(tai_ctx_t *ctx, void (*status_cb)(const char *))
{
    g_ctx = ctx;
    g_status_cb = status_cb;
    /* 640x480 足够多模态理解，又不至于让 rv32 软编码太慢。
     * 走 bsp，不直接碰 V4L2 —— 换相机后端只改 bsp/boards/。 */
    const bsp_t *b = bsp_get();
    g_cam_ok = (b->camera_start && b->camera_start(640, 480) == 0);
    if (!g_cam_ok)
        fprintf(stderr, "[MCP] 相机初始化失败 —— take_photo 工具仍会注册，"
                        "但调用时会返回失败\n");
    g_quit = 0;
    if (pthread_create(&g_thr, NULL, photo_thread, NULL) != 0) {
        fprintf(stderr, "[MCP] 拍照线程创建失败\n");
        return -1;
    }
    printf("[MCP] 已注册工具：take_photo%s\n", g_cam_ok ? "" : "（相机不可用）");
    return 0;
}

void mcp_stop(void)
{
    g_quit = 1;
    pthread_join(g_thr, NULL);
    if (g_cam_ok) bsp_get()->camera_stop();
}

/* -- 协议应答 ------------------------------------------------------------ */

/* 工具目录。description 写清楚"什么时候该调"，大模型据此判断。 */
static const char *TOOLS_JSON =
    "{"
      "\"name\":\"take_photo\","
      "\"description\":\"用设备摄像头拍一张照片并上传，让你能看到设备前方的画面。"
      "当用户说'看看有什么'、'这是什么'、'看一下'、'帮我看看'等需要视觉才能回答的"
      "问题时调用。调用后图片会通过图像通道发给你。\","
      "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
        "\"hint\":{\"type\":\"string\",\"description\":\"希望重点看什么，可省略\"}"
      "},\"required\":[]}"
    "}";

void mcp_handle(tai_ctx_t *ctx, const char *json)
{
    if (!json) return;

    char method[64] = {0};
    json_get_string(json, "method", method, sizeof(method));

    /* id 可能是数字也可能是字符串，原样回带最稳；取不到就用 null */
    char idbuf[80] = "null";
    char ids[64] = {0};
    if (json_get_string(json, "id", ids, sizeof(ids)) == 0 && ids[0]) {
        snprintf(idbuf, sizeof(idbuf), "\"%s\"", ids);
    } else {
        long idn = 0;
        if (json_get_long(json, "id", &idn) == 0)
            snprintf(idbuf, sizeof(idbuf), "%ld", idn);
    }

    static char resp[2048];

    if (strcmp(method, "initialize") == 0) {
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
              "\"protocolVersion\":\"%s\","
              "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"},"
              "\"capabilities\":{\"tools\":{}}}}",
            idbuf, MCP_PROTO, MCP_NAME, MCP_VER);

    } else if (strcmp(method, "tools/list") == 0) {
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":[%s]}}",
            idbuf, TOOLS_JSON);
        printf("[MCP] 上报工具目录（take_photo）\n");

    } else if (strcmp(method, "tools/call") == 0) {
        char *params = json_get_object(json, "params");
        char name[64] = {0};
        if (params) json_get_string(params, "name", name, sizeof(name));

        if (strcmp(name, "take_photo") == 0) {
            g_photo_hint[0] = '\0';
            if (params) {
                char *args = json_get_object(params, "arguments");
                if (args) {
                    json_get_string(args, "hint", g_photo_hint, sizeof(g_photo_hint));
                    free(args);
                }
            }
            printf("[MCP] 大模型请求拍照%s%s\n",
                   g_photo_hint[0] ? "，提示语：" : "", g_photo_hint);
            g_photo_req = 1;          /* 交给 photo 线程，立刻返回不阻塞 */
            snprintf(resp, sizeof(resp),
                "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":"
                "[{\"type\":\"text\",\"text\":\"已拍照，图片正在上传，"
                "请根据随后收到的图片回答\"}],\"isError\":false}}", idbuf);
        } else {
            snprintf(resp, sizeof(resp),
                "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":"
                "[{\"type\":\"text\",\"text\":\"unknown tool\"}],\"isError\":true}}",
                idbuf);
        }
        if (params) free(params);

    } else {
        /* notifications/* 之类不需要结果，回个空 result 保持兼容 */
        snprintf(resp, sizeof(resp),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":"
            "[{\"type\":\"text\",\"text\":\"\"}]}}", idbuf);
    }

    if (tai_send_mcp_response(ctx, resp) != TAI_OK)
        fprintf(stderr, "[MCP] 应答发送失败（method=%s）\n", method);
}
