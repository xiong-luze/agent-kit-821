/*
 * main.c —— 产品层：把各层拼起来
 *
 *   板级初始化 → 屏幕 → 音频 → DSP → 凭据(配网) → 连云 → MCP → 状态机
 *
 * 分层见 docs/architecture.md：
 *   bsp/     板级（V4L2 / fbdev / ALSA 管道 / mixer 配方）
 *   core/    平台无关（状态机 / VAD / AEC / ringbuf / JSON）
 *   service/ 涂鸦云（会话 / 配网 / MCP）
 *   ui/      界面
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "bsp/board.h"
#include "iot_client.h"

#include "../core/app.h"
#include "../core/audio_dsp.h"
#include "../core/ringbuf.h"
#include "../core/presence.h"
#include "../service/session.h"
#include "../service/provision.h"
#include "../service/mcp.h"
#include "../ui/ui.h"

/* -- 设备三元组 -----------------------------------------------------------
 * TUYA_UUID / TUYA_AUTH_KEY / TUYA_PRODUCT_KEY 来自构建期生成的 tuya_authkey.h
 * —— 真实值只存在 gitignore 的 tuya_authkey.txt 里，**绝不写进源文件**。
 * 见 CMakeLists.txt 顶部和 tuya_authkey.txt.example。
 *
 * 三元组只用于首次扫码配网。激活后拿到的 devid/secret_key/local_key 存到
 * /mnt/UDISK/tuya_creds.txt，之后直接读盘，不再需要三元组。
 *
 * --demo-dev 用的那组已激活设备凭据（DEMO_DEVID / DEMO_SECRET_KEY /
 * DEMO_LOCAL_KEY）同样来自那个头，**同样不写进源文件** —— 那是某台真实设备的
 * 凭据，不该由这个仓库传播出去，哪怕它在别处是公开的。
 * 没在 tuya_authkey.txt 里配的话 --demo-dev 直接不可用（HAVE_DEMO_DEV 未定义），
 * 想跳过配网可以把三个值当位置参数传：v821_chatbot <devid> <sk> <lk>。 */
#include "tuya_authkey.h"

static int envi_ms(const char *k, int def)
{
    const char *v = getenv(k);
    return v ? atoi(v) : def;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void usage(const char *p)
{
    printf("用法：%s [devid secret_key local_key]\n", p);
    printf("  --tune       只打印麦克风电平，用来定 VAD 阈值\n");
    printf("  --pdet-tune  只跑人形检测并打印单帧耗时（定采样间隔用）\n");
    printf("  --no-aec     关软件 AEC（播放期间仍会物理关麦）\n");
    printf("  --full-duplex 持续采集+AEC+本地打断（需要alsa后端）\n");
    printf("  --half-duplex 使用原一问一答回退\n");
    printf("  --wake-word 短语  本地唤醒词，默认你好涂鸦\n");
    printf("  --wake-model 目录 --wake-dict 词典  本地中文模型资源\n");
    printf("  --no-wake    全双工只用VAD，不等待唤醒词（AEC联调用）\n");
    printf("  --no-greet   关掉「看到有人主动打招呼」\n");
    printf("  --greet-only 关掉声音触发，只留主动打招呼（吵闹环境下验证用）\n");
    printf("  --reprov     清掉凭据存档，重新扫码配网\n");
    printf("  --demo-dev   用 SDK 例程的测试设备，跳过配网\n");
    printf("环境变量：V821_VAD_START / V821_VAD_END / V821_VAD_HANG /\n");
    printf("          V821_GUARD_MS / V821_AEC_TAIL /\n");
    printf("          V821_GREET(0关) / V821_GREET_IDLE_MS(默认60000) /\n");
    printf("          V821_VOICE_TRIGGER(0=只留主动打招呼) / V821_PDET_*（见 README §5.7）\n");
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    app_cfg_t cfg;
    app_cfg_defaults(&cfg);
    int tune = 0, pdet_tune = 0, reprov = 0, demo_dev = 0, wake_requested = 0;
    for (int i = 1; i < argc; i++) {
        if (i == 1 && argc >= 4 && argv[1][0] != '-') { i = 3; continue; }
        if (!strcmp(argv[i], "--full-duplex")) { cfg.full_duplex = 1; continue; }
        if (!strcmp(argv[i], "--half-duplex")) { cfg.full_duplex = 0; continue; }
        if (!strcmp(argv[i], "--no-wake")) { cfg.wake_enabled = 0; continue; }
        if (!strcmp(argv[i], "--wake-word") || !strcmp(argv[i], "--wake-model") ||
            !strcmp(argv[i], "--wake-dict")) {
            const char *key = argv[i];
            if (++i >= argc) { fprintf(stderr, "缺少%s参数\n", key); return 1; }
            if (!strcmp(key, "--wake-word")) cfg.wake_word = argv[i];
            else if (!strcmp(key, "--wake-model")) cfg.wake_model = argv[i];
            else cfg.wake_dictionary = argv[i];
            wake_requested = 1;
            continue;
        }
        if (!strcmp(argv[i], "--tune"))      { tune = 1; continue; }
        if (!strcmp(argv[i], "--pdet-tune")) { pdet_tune = 1; continue; }
        if (!strcmp(argv[i], "--no-aec"))    { cfg.aec_enable = 0; continue; }
        if (!strcmp(argv[i], "--no-greet"))  { cfg.greet_enable = 0; continue; }
        if (!strcmp(argv[i], "--greet-only")) { cfg.voice_trigger = 0; continue; }
        if (!strcmp(argv[i], "--reprov"))   { reprov = 1; continue; }
        if (!strcmp(argv[i], "--demo-dev")) { demo_dev = 1; continue; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        fprintf(stderr, "未知参数：%s\n", argv[i]);
        return 1;
    }
    if (wake_requested && !cfg.full_duplex) {
        fprintf(stderr, "自定义唤醒词需同时启用 --full-duplex\n"); return 1;
    }
    if (app_cfg_validate(&cfg)) return 1;

    const bsp_t *b = bsp_get();
    if (cfg.full_duplex && !(b->caps & BSP_CAP_AUDIO_DUPLEX)) {
        fprintf(stderr, "请使用 -DV821_AUDIO_BACKEND=alsa 构建全双工版本\n");
        return 1;
    }
    printf("[板级] %s\n", b->name);
    if (b->init && b->init() != BSP_OK) { fprintf(stderr, "板级初始化失败\n"); return 1; }

    ui_start();                    /* 屏幕不可用也继续跑，只是没画面 */
    ui_set_status("启动中...");

    int frame_samples = cfg.sample_rate * cfg.frame_ms / 1000;
    if (dsp_init(cfg.sample_rate, frame_samples, cfg.aec_tail_ms, cfg.aec_enable) && cfg.full_duplex) {
        fprintf(stderr, "全双工AEC初始化失败\n"); return 1;
    }
    if (ringbuf_init(cfg.sample_rate, 40)) {
        fprintf(stderr, "播放缓冲内存分配失败\n"); return 1;
    }

    if (tune) {
        if (b->audio_start(cfg.sample_rate) != BSP_OK) return 1;
        app_tune(&cfg);
        b->audio_stop();
        dsp_deinit();
        return 0;
    }

    /* 人形检测跑分：不连云，只要相机。单帧耗时决定采样间隔能设多小。 */
    if (pdet_tune) {
        if (!b->camera_start || b->camera_start(640, 480) != BSP_OK) {
            fprintf(stderr, "相机启动失败\n"); return 1;
        }
        presence_cfg_t pc;
        presence_cfg_defaults(&pc);
        pc.watch_ms = envi_ms("V821_PDET_WATCH_MS", 0);   /* 跑分默认连着跑 */
        presence_tune(&pc);
        b->camera_stop();
        return 0;
    }

    /* ★ PAL 必须在任何 iot_* 之前初始化 —— 扫码配网的 iot_get_qrcode_info()
     * 也要它，所以放在凭据解析之前。重复调用安全（只是设 PAL 和 cJSON hooks）。 */
    iot_init_default();

    /* ---- 凭据：命令行 > --demo-dev > 存盘 > 扫码配网 -------------------- */
    static prov_creds_t creds;
    const char *devid, *sk, *lk;

    if (reprov) prov_erase();

    if (argc >= 4 && argv[1][0] != '-') {
        devid = argv[1]; sk = argv[2]; lk = argv[3];
        printf("[凭据] 用命令行传入的\n");
    } else if (demo_dev) {
#ifdef HAVE_DEMO_DEV
        devid = DEMO_DEVID; sk = DEMO_SECRET_KEY; lk = DEMO_LOCAL_KEY;
        printf("[凭据] 用 tuya_authkey.txt 里的 DEMO_* 设备（--demo-dev）\n");
#else
        fprintf(stderr, "[凭据] --demo-dev 不可用：tuya_authkey.txt 里没配 DEMO_DEVID/"
                        "DEMO_SECRET_KEY/DEMO_LOCAL_KEY（见 .example）。\n"
                        "       也可以直接传位置参数：%s <devid> <sk> <lk>\n", argv[0]);
        return 1;
#endif
    } else if (prov_load(&creds)) {
        devid = creds.devid; sk = creds.secret_key; lk = creds.local_key;
        printf("[凭据] 读自存档，devid=%s\n", devid);
    } else {
        printf("[凭据] 没有存档，开始扫码配网\n");
        ui_set_status("首次使用，需要配网");
        if (prov_run_qr(TUYA_UUID, TUYA_AUTH_KEY, TUYA_PRODUCT_KEY,
                        &creds, ui_set_status, ui_show_qr) != 0) {
            fprintf(stderr, "[凭据] 配网失败。可用 --demo-dev 先跑测试设备，"
                            "或检查三元组是否与涂鸦平台上的产品匹配\n");
            ui_show_qr(NULL);
            return 1;
        }
        ui_show_qr(NULL);
        devid = creds.devid; sk = creds.secret_key; lk = creds.local_key;
    }

    session_ui_t sui = {
        .on_user_text = ui_set_user,
        .on_ai_text   = ui_ai_append,
        .on_emotion   = ui_set_emotion,
        .on_status    = ui_set_status,
        .on_turn_begin = ui_ai_begin,
    };
    int result = 0;
    unsigned retry_delay_s = 1, retry_attempt = 0, audio_rebuilds = 0;
    for (;;) {
        if (session_connect(devid, sk, lk, &sui) != 0) {
            /* Connect can fail after allocating IoT/TLS resources. */
            session_close();
            result = -1;
            if (!cfg.full_duplex) break;
        } else {
            retry_delay_s = 1;
            mcp_start(session_ctx(), ui_set_status);
            int64_t run_started_ms = monotonic_ms();
            if (cfg.full_duplex)
                result = dsp_reset() ? APP_DUPLEX_ERROR : app_run_duplex(&cfg);
            else if (b->audio_start(cfg.sample_rate) != BSP_OK) result = -1;
            else app_run(&cfg);
            int64_t run_ended_ms = monotonic_ms();
            if (run_started_ms >= 0 && run_ended_ms - run_started_ms >= 30000)
                audio_rebuilds = 0;
            if (cfg.full_duplex && result == APP_DUPLEX_REBUILD_AUDIO) {
                int allowed = audio_rebuilds < 3;
                fprintf(stderr, "[音频恢复诊断] recent_rebuilds=%u allowed=%d\n", audio_rebuilds, allowed);
                if (allowed) audio_rebuilds++;
                else {
                    result = APP_DUPLEX_ERROR;
                    ui_set_status("音频连续异常，请检查设备");
                }
            }

            /* Audio already stopped before its sender was joined. Retire all
             * old callbacks/photo work before replacing the SDK context. */
            mcp_stop();
            session_close();
            if (!cfg.full_duplex || (result != APP_DUPLEX_RECONNECT && result != APP_DUPLEX_REBUILD_AUDIO)) break;
        }
        ringbuf_clear();
        ui_set_status(result == APP_DUPLEX_REBUILD_AUDIO ? "音频中断，恢复中..." : "网络连接中断，重连中...");
        fprintf(stderr, "[重连诊断] attempt=%u delay_s=%u cause=%d\n",
                ++retry_attempt, retry_delay_s, result);
        struct timespec pause = {.tv_sec = retry_delay_s, .tv_nsec = 0};
        while (nanosleep(&pause, &pause) && errno == EINTR) {}
        if (retry_delay_s < 5) {
            retry_delay_s *= 2;
            if (retry_delay_s > 5) retry_delay_s = 5;
        }
    }

    ringbuf_free();
    dsp_deinit();
    ui_stop();
    b->audio_stop();
    if (b->display_stop) b->display_stop();
    return result ? 1 : 0;
}
