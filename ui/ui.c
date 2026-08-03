/* ui_fb.c —— 见 ui_fb.h 的说明 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#include "lvgl.h"
#include "bsp/board.h"
#include "assets/emoji/emoji_table.h"
#include "qrcodegen.h"

/* -- tick ---------------------------------------------------------------- */
uint32_t ui_tick_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/* -- 外部写入的文字缓冲（加锁保护，UI 线程读） --------------------------- */
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_status[128], g_user[512], g_ai[1024], g_emo[32];
static int  g_dirty = 0;
static volatile int g_run = 0, g_ok = 0;
static pthread_t g_thr;

/* 配网二维码占屏中：UI 线程此时不重画，免得把二维码盖掉。定义在 ui_qr.c。 */
extern volatile int g_ui_qr_on;

int  ui_screen_w(void);
int  ui_screen_h(void);


/* -- LVGL 对象 ----------------------------------------------------------- */
static lv_obj_t *l_status, *l_user, *l_ai, *l_emo;

#define SCR_W 320
#define SCR_H 240

/* 显示走 bsp：上层只交出 RGB565 缓冲，怎么送到屏上（SPI 屏要手动
 * FBIOPAN_DISPLAY）是板子的事，见 bsp/boards/v821_aitoy/display_fb.c。 */
static int g_w = 320, g_h = 240;

static void fb_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    /* full_refresh=1，area 就是整屏 */
    bsp_get()->display_flush(px, area->x1, area->y1,
                             area->x2 - area->x1 + 1, area->y2 - area->y1 + 1);
    lv_disp_flush_ready(drv);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101010), 0);
    lv_obj_set_style_pad_all(scr, 6, 0);

    /* 顶部状态行 */
    l_status = lv_label_create(scr);
    lv_obj_set_width(l_status, SCR_W - 12);
    lv_obj_set_style_text_color(l_status, lv_color_hex(0x808080), 0);
    lv_label_set_long_mode(l_status, LV_LABEL_LONG_DOT);
    lv_obj_align(l_status, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(l_status, "启动中...");

    /* 情绪（右上角）：用 32x32 图，不用文字。
     * puhui 字体没有 emoji 字形，文字里的 emoji 只会渲染成豆腐块，所以
     * 文字侧一律剥离，改在这里用官方素材（已转成 RGB565+A8）表现。 */
    l_emo = lv_img_create(scr);
    lv_obj_align(l_emo, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(l_emo, LV_OBJ_FLAG_HIDDEN);

    /* 你说的话 */
    l_user = lv_label_create(scr);
    lv_obj_set_width(l_user, SCR_W - 12);
    lv_label_set_long_mode(l_user, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l_user, lv_color_hex(0x4FC3F7), 0);
    lv_obj_align(l_user, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_label_set_text(l_user, "");

    /* AI 回答（占剩下的空间，超出滚动到底部） */
    l_ai = lv_label_create(scr);
    lv_obj_set_width(l_ai, SCR_W - 12);
    lv_label_set_long_mode(l_ai, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(l_ai, lv_color_hex(0xE8E8E8), 0);
    lv_obj_align(l_ai, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_label_set_text(l_ai, "");
}

static void *ui_thread(void *arg)
{
    (void)arg;
    while (g_run) {
        if (g_ui_qr_on) { usleep(100 * 1000); continue; }   /* 配网二维码占屏中 */
        pthread_mutex_lock(&g_mtx);
        if (g_dirty) {
            g_dirty = 0;
            lv_label_set_text(l_status, g_status);
            lv_label_set_text(l_user,   g_user);
            lv_label_set_text(l_ai,     g_ai);
            if (g_emo[0]) {
                const lv_img_dsc_t *im = NULL;
                for (int i = 0; i < EMOJI_TABLE_N; i++)
                    if (strcmp(EMOJI_TABLE[i].name, g_emo) == 0) {
                        im = EMOJI_TABLE[i].img; break;
                    }
                if (im) {
                    lv_img_set_src(l_emo, im);
                    lv_obj_clear_flag(l_emo, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(l_emo, LV_OBJ_FLAG_HIDDEN);
                }
            }
            /* AI 文字长了就把标签往上顶，保证最新的一行可见 */
            lv_coord_t h = lv_obj_get_height(l_ai);
            lv_coord_t y = 70;
            if (h > SCR_H - 70 - 6) y = SCR_H - 6 - h;
            lv_obj_align(l_ai, LV_ALIGN_TOP_LEFT, 0, y);
        }
        pthread_mutex_unlock(&g_mtx);
        lv_timer_handler();
        usleep(30 * 1000);
    }
    return NULL;
}

int ui_start(void)
{
    const bsp_t *b = bsp_get();
    if (!b->display_start || b->display_start(&g_w, &g_h) != 0) return -1;
    if (g_w != SCR_W || g_h != SCR_H)
        printf("[屏幕] 实际 %dx%d，界面按 %dx%d 布局\n", g_w, g_h, SCR_W, SCR_H);

    lv_init();

    /* 整屏缓冲 + full_refresh：SPI 屏每次推屏都是整帧，分块刷没有意义，
     * 而且整屏刷每帧只需要一次 FBIOPAN_DISPLAY。150KB 静态，内存够。 */
    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf[SCR_W * SCR_H];
    lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCR_W * SCR_H);

    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.draw_buf     = &draw_buf;
    drv.flush_cb     = fb_flush;
    drv.hor_res      = SCR_W;
    drv.ver_res      = SCR_H;
    drv.full_refresh = 1;
    lv_disp_drv_register(&drv);

    build_ui();

    snprintf(g_status, sizeof(g_status), "启动中...");
    g_dirty = 1;
    g_run = 1; g_ok = 1;
    if (pthread_create(&g_thr, NULL, ui_thread, NULL) != 0) {
        fprintf(stderr, "[屏幕] UI 线程创建失败\n");
        g_run = 0; g_ok = 0;
        return -1;
    }
    printf("[屏幕] LVGL 已就绪（%dx%d RGB565，字体 puhui 14px）\n", SCR_W, SCR_H);
    return 0;
}

void ui_stop(void)
{
    if (!g_ok) return;
    g_run = 0;
    pthread_join(g_thr, NULL);
}

/* -- setters（只写缓冲，绝不碰 LVGL） ------------------------------------ */
#define SETTER(fn, buf)                                   \
    void fn(const char *s) {                              \
        if (!g_ok || !s) return;                          \
        pthread_mutex_lock(&g_mtx);                       \
        snprintf(buf, sizeof(buf), "%s", s);              \
        g_dirty = 1;                                      \
        pthread_mutex_unlock(&g_mtx);                     \
    }
SETTER(ui_set_status,   g_status)
SETTER(ui_set_emotion,  g_emo)

void ui_set_user(const char *s)
{
    if (!g_ok || !s) return;
    pthread_mutex_lock(&g_mtx);
    snprintf(g_user, sizeof(g_user), "你：%s", s);
    g_dirty = 1;
    pthread_mutex_unlock(&g_mtx);
}

void ui_ai_begin(void)
{
    if (!g_ok) return;
    pthread_mutex_lock(&g_mtx);
    g_ai[0] = '\0';
    g_dirty = 1;
    pthread_mutex_unlock(&g_mtx);
}

void ui_ai_append(const char *s)
{
    if (!g_ok || !s) return;
    pthread_mutex_lock(&g_mtx);
    size_t n = strlen(g_ai), m = strlen(s);
    if (n + m < sizeof(g_ai) - 1) { memcpy(g_ai + n, s, m); g_ai[n + m] = '\0'; }
    g_dirty = 1;
    pthread_mutex_unlock(&g_mtx);
}

/* -- 给 ui_qr.c 用 -------------------------------------------------------- */
int ui_screen_w(void) { return g_w; }
int ui_screen_h(void) { return g_h; }

/* 二维码收起后，让 UI 线程整屏重画一次 */
void ui_redraw(void)
{
    if (!g_ok) return;
    pthread_mutex_lock(&g_mtx);
    g_dirty = 1;
    pthread_mutex_unlock(&g_mtx);
    lv_obj_invalidate(lv_scr_act());
}
