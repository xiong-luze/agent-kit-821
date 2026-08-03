/*
 * presence.c —— 有人靠近检测
 *
 * ★★ 先看这个：判据是「窗口命中率」，不是「连续 N 帧」★★
 * 最早一版喂的是最近邻缩放 + 拉伸到 5:3 的图，人站着不动 35 帧只检到 13 帧
 * （37%），结果 1,1,0,0,1,1,0,... 乱跳，「连续 3 秒都检到」这个条件几乎永远
 * 不成立。改成面积平均 + letterbox 之后实测有人约 100%、空场景约 9%（孤立
 * 单帧），但判据仍然保留命中率形式 —— 真人会动、会侧身、光线会变，不能假设
 * 单帧永远可靠。
 *
 * 两级采样，兼顾 CPU 和判断可靠性：
 *
 *   巡视期 WATCH：每 watch_ms(1s) 抓一帧。CPU 占用 = 167/1000 ≈ 17%。
 *      │  任意一帧检到人（宁可误入，反正下一级会否掉）
 *      ▼
 *   确认期 CONFIRM：连着抓帧（不留间隔）confirm_ms(3s)，统计命中率。
 *      │  命中率 >= confirm_ratio，且距上次打招呼 >= min_gap_ms
 *      ▼
 *   触发一次（置 trigger，主循环取走）→ 静默期 FIRED
 *
 *   静默期 FIRED：人还在 / 刚打过招呼。
 *      │  连续没人累计 >= rearm_quiet_ms(30s)
 *      ▼
 *   回到 WATCH
 *
 * 确认期连着抓帧是故意的：3 秒能取到 9~18 个样本，统计上才分得开「有人 100%」
 * 和「空场景 9%」。每秒一帧的话 3 秒只有 3 个样本，判不出来。
 * 代价是那 3 秒 CPU 满载，但确认期很短、而且只在待机时发生。
 *
 * ★ 实测：连着跑单帧 167ms，但**稀疏采样时会一直停在慢档** —— 巡视期一秒一帧
 * 的情况下，确认期 3 秒只取到 9 帧（单帧约 330ms），不是按 167ms 算的 18 帧。
 * 看着是 CPU 调频没爬上来（330/167 ≈ 2）。所以 CONFIRM_MIN_SAMPLES 不能定太高，
 * 否则确认期永远"样本不足"；CPU 占用也要按 31% 估而不是 17%。
 *
 * 为什么不是「每隔一分钟检到人就打招呼」：人坐在镜头前不动就会每分钟被叫
 * 一次。所以是**边缘触发** —— 必须"人离开再回来"才重新触发。
 *
 * 计时一律用**时间**而不是帧数：单帧耗时随画面复杂度变，用帧数会让
 * "3 秒"变成一个漂移的值。
 *
 * ★ CPU 约束：纯 CPU 软推理（这芯片没 NPU），和音频主循环抢同一个核，而音频
 * 是 20ms 硬节拍 —— 被抢了就掉帧、AEC 失效、云端断流（这类坑踩过好几次）。
 * 所以对话期间由上层 presence_set_enabled(0) 彻底停掉，而且本线程自己
 * nice +10，让音频线程随时抢得过它。
 */
#include "presence.h"
#include "bsp/board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifdef HAVE_PDET
#include "aw_person_detection_v821.h"
#include "aw_image.h"
#endif

static int64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int envi(const char *k, int def)
{
    const char *v = getenv(k);
    return v ? atoi(v) : def;
}

void presence_cfg_defaults(presence_cfg_t *c)
{
    c->watch_ms       = envi("V821_PDET_WATCH_MS", 1000);  /* 巡视 1s/帧 ≈17% CPU */
    c->confirm_ms     = envi("V821_PDET_CONFIRM_MS", 3000);/* 确认连抓 3s ≈18 帧 */
    c->rearm_quiet_ms = envi("V821_PDET_REARM_MS", 30000); /* 走开 30s 才重新武装 */
    c->min_gap_ms     = envi("V821_PDET_GAP_MS", 180000);  /* 两次招呼至少 3 分钟 */
    c->det_w          = 320;      /* 模型固定输入 */
    c->det_h          = 192;
    /* 百分比传参，避免环境变量里写浮点 */
    /* 实测（letterbox+面积平均后）：有人时命中率约 100%，空场景约 9%（且都是
     * 孤立单帧）。60% 落在中间，两边余量都够：真人会动、会侧身、光线会变，
     * 不能假设一直 100%；而空场景要凑出 11/18 的误检统计上不可能。 */
    c->confirm_ratio  = envi("V821_PDET_RATIO", 60) / 100.0f;
    c->threshold      = envi("V821_PDET_TH", 30) / 100.0f;     /* 官方默认 0.3 */
    /* 相机 4:3 直接压到模型的 5:3 会把人竖着挤扁 1.25 倍。默认加灰边保比例，
     * V821_PDET_FIT=stretch 可以退回拉伸做对照。 */
    const char *fit = getenv("V821_PDET_FIT");
    c->letterbox = (fit && !strcmp(fit, "stretch")) ? 0 : 1;
}

static presence_cfg_t  s_cfg;
static pthread_t       s_thd;
static volatile int    s_run     = 0;
static volatile int    s_enabled = 0;
static volatile int    s_rearm   = 0;   /* 置位表示"刚开启，需要先满足无人期" */
static volatile int    s_trigger = 0;

#ifdef HAVE_PDET
static AW_Det_Container *s_ctn = NULL;
static AW_Det_Outputs   *s_out = NULL;
static uint8_t          *s_det = NULL;   /* 喂给模型的 det_w*det_h*3 */
static uint8_t          *s_tmp = NULL;   /* letterbox 时的内层缓冲 */
static int               s_in_w = 0, s_in_h = 0;   /* letterbox 内层尺寸 */
static int               s_pad_x = 0, s_pad_y = 0;

/* 算 letterbox 的内层尺寸：等比缩到能塞进 det_w×det_h 的最大尺寸，居中。
 * 相机 640x480(4:3) → 模型 320x192(5:3)，直接拉伸会把人竖着挤扁 1.25 倍，
 * 这对人形检测器是实打实的分布偏移。 */
static void calc_letterbox(void)
{
    const bsp_t *b = bsp_get();
    int sw = 0, sh = 0;
    if (b->camera_get_size) b->camera_get_size(&sw, &sh);
    if (sw <= 0 || sh <= 0 || !s_cfg.letterbox) {
        s_in_w = s_cfg.det_w; s_in_h = s_cfg.det_h;
        s_pad_x = s_pad_y = 0;
        return;
    }
    /* 比较 sw/sh 与 det_w/det_h，用整数交叉相乘避免浮点 */
    if (sw * s_cfg.det_h >= sh * s_cfg.det_w) {   /* 源更宽：以宽为准 */
        s_in_w = s_cfg.det_w;
        s_in_h = sh * s_cfg.det_w / sw;
    } else {                                      /* 源更高：以高为准 */
        s_in_h = s_cfg.det_h;
        s_in_w = sw * s_cfg.det_h / sh;
    }
    if (s_in_w > s_cfg.det_w) s_in_w = s_cfg.det_w;
    if (s_in_h > s_cfg.det_h) s_in_h = s_cfg.det_h;
    s_pad_x = (s_cfg.det_w - s_in_w) / 2;
    s_pad_y = (s_cfg.det_h - s_in_h) / 2;
    printf("[presence] letterbox：相机 %dx%d → 内层 %dx%d，居中放进 %dx%d\n",
           sw, sh, s_in_w, s_in_h, s_cfg.det_w, s_cfg.det_h);
}

/* 抓一帧跑一次检测。返回检到的人数，<0 表示取帧/检测失败。
 * cost_ms 可为 NULL；非 NULL 时写回单帧推理耗时（不含取帧和缩放）。 */
static int detect_once(int *cost_ms)
{
    const bsp_t *b = bsp_get();
    if (!b->camera_capture_rgb) return -1;

    if (s_in_w == s_cfg.det_w && s_in_h == s_cfg.det_h) {
        if (b->camera_capture_rgb(s_det, s_cfg.det_w, s_cfg.det_h) != 0) return -1;
    } else {
        if (b->camera_capture_rgb(s_tmp, s_in_w, s_in_h) != 0) return -1;
        memset(s_det, 114, (size_t)s_cfg.det_w * s_cfg.det_h * 3);  /* YOLO 惯用灰 */
        for (int y = 0; y < s_in_h; y++)
            memcpy(s_det + ((size_t)(y + s_pad_y) * s_cfg.det_w + s_pad_x) * 3,
                   s_tmp + (size_t)y * s_in_w * 3,
                   (size_t)s_in_w * 3);
    }

    AW_Img img = aw_make_rgb_image(s_det, s_cfg.det_w, s_cfg.det_h, 3);
    int64_t t0 = now_ms();
    aw_run_person_det(s_ctn, &img, s_out, s_cfg.threshold);
    if (cost_ms) *cost_ms = (int)(now_ms() - t0);
    int n = s_out->num;
    aw_free_image(&img);
    return n;
}

/* 命中率至少要有这么多样本才算得数。确认期可能被中途打断（上层进对话就会
 * s_enabled=0），只剩一两帧的话"1 帧命中 1 帧 = 100%"会直接骗过门限。 */
#define CONFIRM_MIN_SAMPLES 5

/* 确认期：连着抓帧 confirm_ms，返回命中率（0~1）。<0 表示样本不足，不作结论。 */
static float confirm_burst(void)
{
    int64_t t0 = now_ms();
    int hit = 0, tot = 0;
    while (now_ms() - t0 < s_cfg.confirm_ms && s_run && s_enabled) {
        int n = detect_once(NULL);
        if (n < 0) continue;
        tot++;
        if (n > 0) hit++;
    }
    if (tot < CONFIRM_MIN_SAMPLES) {
        printf("[presence] 确认期只取到 %d 帧（<%d），不作结论\n",
               tot, CONFIRM_MIN_SAMPLES);
        return -1.0f;
    }
    printf("[presence] 确认期 %d 帧命中 %d（%.0f%%，门限 %.0f%%）\n",
           tot, hit, 100.0 * hit / tot, 100.0 * s_cfg.confirm_ratio);
    return (float)hit / (float)tot;
}

static void *presence_thread(void *arg)
{
    (void)arg;
    /* 降优先级：音频线程是 20ms 硬节拍，必须随时抢得过检测 */
    setpriority(PRIO_PROCESS, 0, 10);

    enum { ST_WATCH, ST_FIRED } st = ST_FIRED;   /* 开机先要求一段无人期 */
    int64_t quiet_since = now_ms(), last_greet = 0;

    while (s_run) {
        if (!s_enabled) {
            quiet_since = 0;              /* 关闭期间不累计无人时长 */
            usleep(200 * 1000);
            continue;
        }
        if (s_rearm) {                    /* 上层要求重新武装：回到"需要无人期"那档 */
            s_rearm = 0;
            st = ST_FIRED;
            quiet_since = now_ms();
        }

        int n = detect_once(NULL);
        int64_t now = now_ms();

        if (st == ST_WATCH) {
            if (n > 0) {                  /* 巡视期宁可误入，让确认期去否掉 */
                if (last_greet && now - last_greet < s_cfg.min_gap_ms) {
                    /* 还在最小间隔里，不值得为它烧 3 秒 CPU */
                } else if (confirm_burst() >= s_cfg.confirm_ratio) {
                    printf("[presence] 确认有人 → 主动打招呼\n");
                    s_trigger  = 1;
                    last_greet = now_ms();
                    st = ST_FIRED;
                    quiet_since = 0;
                    continue;             /* 不再睡 watch_ms，立刻进静默期 */
                }
            }
        } else {                          /* ST_FIRED：等视野空够久 */
            if (n > 0) {
                quiet_since = 0;
            } else if (n == 0) {
                if (!quiet_since) quiet_since = now;
                if (now - quiet_since >= s_cfg.rearm_quiet_ms) {
                    printf("[presence] 视野已空 %d 秒，重新武装\n",
                           s_cfg.rearm_quiet_ms / 1000);
                    st = ST_WATCH;
                }
            }
            /* n < 0：取帧失败，什么都不动 */
        }

        usleep((useconds_t)s_cfg.watch_ms * 1000);   /* 把核让出去 */
    }
    return NULL;
}
#endif /* HAVE_PDET */

int presence_start(const presence_cfg_t *c)
{
    s_cfg = *c;
#ifndef HAVE_PDET
    printf("[presence] 未编入人形检测库（third_party/pdet 缺失），功能关闭\n");
    return -1;
#else
    const bsp_t *b = bsp_get();
    if (!(b->caps & BSP_CAP_CAMERA) || !b->camera_capture_rgb) {
        printf("[presence] 本板没有相机，功能关闭\n");
        return -1;
    }

    size_t px = (size_t)s_cfg.det_w * s_cfg.det_h * 3;
    s_det = (uint8_t *)malloc(px);
    s_tmp = (uint8_t *)malloc(px);
    s_ctn = (AW_Det_Container *)calloc(1, sizeof(AW_Det_Container));
    s_out = (AW_Det_Outputs   *)calloc(1, sizeof(AW_Det_Outputs));
    if (!s_det || !s_tmp || !s_ctn || !s_out) {
        fprintf(stderr, "[presence] 内存不足\n");
        goto fail;
    }
    calc_letterbox();

    int64_t t0 = now_ms();
    if (aw_init_person_det(s_ctn, s_out, s_cfg.det_w, s_cfg.det_h, 3,
                           s_cfg.threshold) == -1) {
        fprintf(stderr, "[presence] 检测器初始化失败（很可能是内存不够）\n");
        goto fail;
    }
    printf("[presence] 检测器就绪，%dx%d，初始化 %lld ms\n",
           s_cfg.det_w, s_cfg.det_h, (long long)(now_ms() - t0));

    s_run = 1;
    if (pthread_create(&s_thd, NULL, presence_thread, NULL) != 0) {
        fprintf(stderr, "[presence] 起线程失败\n");
        s_run = 0;
        aw_free_person_det(s_ctn, s_out);
        goto fail;
    }
    return 0;

fail:
    free(s_det); s_det = NULL;
    free(s_tmp); s_tmp = NULL;
    free(s_ctn); s_ctn = NULL;
    free(s_out); s_out = NULL;
    return -1;
#endif
}

void presence_stop(void)
{
#ifdef HAVE_PDET
    if (!s_run) return;
    s_run = 0;
    pthread_join(s_thd, NULL);
    aw_free_person_det(s_ctn, s_out);
    free(s_det); s_det = NULL;
    free(s_tmp); s_tmp = NULL;
    free(s_ctn); s_ctn = NULL;
    free(s_out); s_out = NULL;
#endif
}

void presence_set_enabled(int on)
{
    if (!on) s_trigger = 0;      /* 丢掉进对话前攒下的触发 */
    s_enabled = on ? 1 : 0;
}

void presence_rearm(void)
{
    s_rearm   = 1;
    s_trigger = 0;
}

int presence_take_trigger(void)
{
    if (!s_trigger) return 0;
    s_trigger = 0;
    return 1;
}

void presence_tune(const presence_cfg_t *c)
{
#ifndef HAVE_PDET
    (void)c;
    printf("[presence] 未编入人形检测库，无法跑分\n");
#else
    if (presence_start(c) != 0) return;
    s_enabled = 0;                       /* 线程只睡，这里自己连着跑 */
    printf("[presence] 跑分模式（letterbox=%d 门限=%.2f）。\n", s_cfg.letterbox,
           (double)s_cfg.threshold);
    printf("           分别量两个场景，把命中率报给我：\n");
    printf("             ① 人站在镜头前不动 → 这是「有人」的命中率\n");
    printf("             ② 空场景没人       → 这是误检率\n");
    printf("           confirm_ratio 取两者中间。Ctrl+C 退出。\n");
    printf("           对照实验：V821_PDET_FIT=stretch / V821_PDET_TH=20\n\n");

    int min = 1 << 30, max = 0, sum = 0, k = 0, hit = 0;
    /* 最近 18 帧（约等于一个确认期）的滑动命中率，直接就是判据的样子 */
    int win[18] = { 0 }, wi = 0, wn = 0, wsum = 0;
    while (1) {
        int cost = 0;
        int n = detect_once(&cost);
        if (n < 0) { printf("取帧/检测失败\n"); usleep(500 * 1000); continue; }
        if (cost < min) min = cost;
        if (cost > max) max = cost;
        sum += cost; k++;
        if (n > 0) hit++;

        int v = (n > 0) ? 1 : 0;
        if (wn < (int)(sizeof(win) / sizeof(win[0]))) wn++;
        wsum += v - win[wi];
        win[wi] = v;
        wi = (wi + 1) % (int)(sizeof(win) / sizeof(win[0]));

        printf("第%3d帧 人数=%d 本帧=%4dms 平均=%dms | 命中率 累计=%3d%% 近%d帧=%3d%%\n",
               k, n, cost, sum / k, 100 * hit / k, wn, 100 * wsum / wn);
        usleep((useconds_t)s_cfg.watch_ms * 1000);
    }
#endif
}
