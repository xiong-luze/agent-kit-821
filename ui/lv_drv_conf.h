/* LVGL 驱动配置：只用标准 Linux framebuffer（/dev/fb0）。
 * 板子的 SPI 屏（ST7789S）由 bsp/drivers/video/sunxi/lcd_fb 注册成 fbdev，
 * 所以用通用 fbdev.c 而不是全志的 sunxifb.c（后者依赖 disp2 的能力）。 */
#ifndef LV_DRV_CONF_H
#define LV_DRV_CONF_H
#include "lv_conf.h"
#define USE_FBDEV     1
#define FBDEV_PATH    "/dev/fb0"
#define USE_BSD_FBDEV 0
#endif
