/* mcp_tools.h —— 设备侧 MCP 工具（见 .c 里的说明） */
#ifndef MCP_H
#define MCP_H
#include "tuya_ai.h"
/* 起相机 + 拍照线程，并注册工具。status_cb 可为 NULL（用于把状态显示到屏幕）。 */
int  mcp_start(tai_ctx_t *ctx, void (*status_cb)(const char *));
void mcp_stop(void);
/* 处理一条 MCP JSON-RPC 请求并应答（在 TAI_EVT_MCP_CMD 里调） */
void mcp_handle(tai_ctx_t *ctx, const char *json);
#endif
