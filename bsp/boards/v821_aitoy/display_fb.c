/*
 * display_fb.c —— V821 aitoy 板的显示（/dev/fb0）
 *
 * 屏是 SPI/DBI 的 ST7789S 320×240，内核 lcd_fb 驱动
 * （bsp/drivers/video/sunxi/lcd_fb）把它注册成标准 framebuffer，RGB565。
 *
 * ★ 必须自己调 FBIOPAN_DISPLAY。SPI 屏不像 RGB 并口屏由控制器自动扫描 ——
 * 内核驱动里唯一的推屏入口就是 fb_pan_display（内部 set_layer + wait_for_vsync），
 * 而且**没有后台刷新线程**。只往 mmap 内存里写，画面永远不会出现在屏上。
 * （LVGL 自带的 lv_drivers/display/fbdev.c 就是只写不 pan，所以我们不用它。）
 */
#include "display_fb.h"
#include "board_config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static int      g_fd  = -1;
static uint8_t *g_mem = NULL;
static struct fb_var_screeninfo g_vinfo;
static struct fb_fix_screeninfo g_finfo;

int board_display_start(int *w, int *h)
{
    int fd = open(BOARD_FB_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[屏幕] 打不开 %s（%s）\n"
                        "       内核可能没编 lcd_fb 驱动，或屏幕未使能。\n"
                        "       板上确认：ls -l /dev/fb* ; dmesg | grep -i 'lcd\\|fb'\n",
                BOARD_FB_DEV, strerror(errno));
        return -1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &g_vinfo) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &g_finfo) != 0) {
        fprintf(stderr, "[屏幕] 读 fb 参数失败\n");
        close(fd); return -1;
    }
    printf("[屏幕] %s %ux%u %ubpp  line=%u  R%u/%u G%u/%u B%u/%u  显存=%u KB\n",
           BOARD_FB_DEV, g_vinfo.xres, g_vinfo.yres, g_vinfo.bits_per_pixel,
           g_finfo.line_length,
           g_vinfo.red.offset, g_vinfo.red.length,
           g_vinfo.green.offset, g_vinfo.green.length,
           g_vinfo.blue.offset, g_vinfo.blue.length, g_finfo.smem_len / 1024);
    if (g_vinfo.bits_per_pixel != 16)
        printf("[屏幕] ⚠ 实际 %ubpp，上层按 RGB565(16bpp) 画 —— 颜色会不对，"
               "需改 ui/lv_conf.h 的 LV_COLOR_DEPTH 重编\n", g_vinfo.bits_per_pixel);

    /* 官方 xstar 被杀后屏可能是 blank 状态 */
    if (ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK) != 0)
        printf("[屏幕] FBIOBLANK 不支持或失败，继续\n");

    g_mem = (uint8_t *)mmap(NULL, g_finfo.smem_len, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (g_mem == MAP_FAILED) {
        fprintf(stderr, "[屏幕] mmap 失败：%s\n", strerror(errno));
        g_mem = NULL; close(fd); return -1;
    }
    g_fd = fd;

    /* 先整屏刷黑推一次 —— 这一步立刻验证推屏通路有没有效 */
    memset(g_mem, 0, g_finfo.smem_len);
    ioctl(g_fd, FBIOPAN_DISPLAY, &g_vinfo);

    if (w) *w = (int)g_vinfo.xres;
    if (h) *h = (int)g_vinfo.yres;
    return 0;
}

void board_display_flush(const void *rgb565, int x, int y, int w, int h)
{
    if (!g_mem || !rgb565) return;
    const uint8_t *src = (const uint8_t *)rgb565;
    for (int row = 0; row < h; row++) {
        memcpy(g_mem + (size_t)(y + row) * g_finfo.line_length + (size_t)x * 2,
               src + (size_t)row * w * 2, (size_t)w * 2);
    }
    ioctl(g_fd, FBIOPAN_DISPLAY, &g_vinfo);   /* ★ 不调这个屏上什么都不会有 */
}

void board_display_stop(void)
{
    if (g_mem) { munmap(g_mem, g_finfo.smem_len); g_mem = NULL; }
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
}
