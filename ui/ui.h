/*
 * ui_fb.h —— 屏幕上显示对话文字（LVGL 8.3 + /dev/fb0）
 *
 * 板子的屏是 SPI/DBI 的 ST7789S 320×240，由内核 lcd_fb 驱动
 * (bsp/drivers/video/sunxi/lcd_fb) 注册成标准 framebuffer，像素格式
 * RGB565（板级 dts 的 lcd_pixel_fmt=0x0a），所以用 LVGL 通用的 fbdev
 * 驱动即可，不需要全志的 sunxifb（那个依赖 disp2 的能力）。
 * 中文字体用官方 chatbox 里现成的阿里普惠体 14px/1bpp。
 *
 * 线程模型：LVGL 不是线程安全的，而文字来自 TAI 的接收线程、音频跑在
 * 主循环里。所以这里自己起一条 UI 线程，外部只调下面这几个 setter
 * （内部加锁、只写字符串缓冲），UI 线程每 30ms 把缓冲刷进 LVGL。
 * **绝不在音频主循环里碰 LVGL** —— 今天已经吃过够多"主循环里做阻塞事"的亏。
 */
#ifndef UI_FB_H
#define UI_FB_H

#include <stdint.h>

/* LVGL 的 tick 源（lv_conf.h 里 LV_TICK_CUSTOM_SYS_TIME_EXPR 指向它） */
uint32_t ui_tick_ms(void);

/* 启动 UI（打开 fb、初始化 LVGL、建界面、起 UI 线程）。
 * 返回 0 成功；非 0 表示屏幕不可用（会打印原因），此时其余接口变成空操作，
 * 程序照常跑，只是没有画面。 */
int  ui_start(void);
void ui_stop(void);

/* 状态行（连云中 / 聆听 / 播放…） */
void ui_set_status(const char *s);
/* 用户这句话（ASR 流式结果，反复调用即可） */
void ui_set_user(const char *s);
/* AI 回答：begin 清空，append 追加（NLG 是分句流式下来的） */
void ui_ai_begin(void);
void ui_ai_append(const char *s);
/* 情绪（SKILL/emo 的 emotion[0]，如 HAPPY） */
void ui_set_emotion(const char *s);

/* 全屏显示配网二维码（qrcodegen 的输出）。传 NULL 关闭、回到对话界面。
 * 直接画进 framebuffer，不经 LVGL —— 二维码要一个模块对齐整数个像素才扫得动，
 * 走 LVGL 的缩放会糊边。 */
void ui_show_qr(const unsigned char *qr);

/* 强制重画一次（二维码关掉后要用它把对话界面刷回来）。 */
void ui_redraw(void);

#endif /* UI_FB_H */
