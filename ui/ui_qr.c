/*
 * ui_qr.c —— 配网二维码
 *
 * ★ 不走 LVGL：二维码必须一个模块对齐整数个像素才扫得动，走 LVGL 的图片
 * 缩放会把边缘糊掉，手机就扫不出来了。这里自己渲染成一整块 RGB565 缓冲，
 * 交给 bsp 一次性推屏。
 */
#include "ui.h"
#include "bsp/board.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 二维码占屏期间，UI 线程不重画（否则对话界面会把它盖掉）。ui.c 里读。 */
volatile int g_ui_qr_on = 0;

/* 由 ui.c 在 ui_start() 里填 */
extern int ui_screen_w(void);
extern int ui_screen_h(void);

void ui_show_qr(const unsigned char *qr)
{
    int W = ui_screen_w(), H = ui_screen_h();

    if (!qr) {                      /* 关掉，让 UI 线程接管重画 */
        g_ui_qr_on = 0;
        ui_redraw();
        return;
    }

    int n     = qrcodegen_getSize(qr);       /* 模块数，通常 25~57 */
    int quiet = 4;                           /* 静默区，扫码必需 */
    int total = n + quiet * 2;
    int scale = (H < W ? H : W) / total;     /* 整数倍，绝不缩放 */
    if (scale < 1) scale = 1;
    int side = total * scale;
    int ox = (W - side) / 2, oy = (H - side) / 2;

    uint16_t *buf = (uint16_t *)malloc((size_t)W * H * 2);
    if (!buf) { fprintf(stderr, "[屏幕] 二维码缓冲分配失败\n"); return; }

    for (int i = 0; i < W * H; i++) buf[i] = 0xFFFF;   /* 白底 */
    for (int my = 0; my < n; my++)
        for (int mx = 0; mx < n; mx++) {
            if (!qrcodegen_getModule(qr, mx, my)) continue;
            int px = ox + (mx + quiet) * scale;
            int py = oy + (my + quiet) * scale;
            for (int dy = 0; dy < scale; dy++)
                for (int dx = 0; dx < scale; dx++)
                    buf[(size_t)(py + dy) * W + px + dx] = 0x0000;
        }

    g_ui_qr_on = 1;
    bsp_get()->display_flush(buf, 0, 0, W, H);
    free(buf);
    printf("[屏幕] 二维码已显示（%d 模块 x%d 倍 = %dpx）\n", n, scale, side);
}
