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

static void usage(const char *p)
{
    printf("用法：%s [devid secret_key local_key]\n", p);
    printf("  --tune       只打印麦克风电平，用来定 VAD 阈值\n");
    printf("  --pdet-tune  只跑人形检测并打印单帧耗时（定采样间隔用）\n");
    printf("  --no-aec     关软件 AEC（播放期间仍会物理关麦）\n");
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
    int tune = 0, pdet_tune = 0, reprov = 0, demo_dev = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tune"))      tune = 1;
        if (!strcmp(argv[i], "--pdet-tune")) pdet_tune = 1;
        if (!strcmp(argv[i], "--no-aec"))    cfg.aec_enable = 0;
        if (!strcmp(argv[i], "--no-greet"))  cfg.greet_enable = 0;
        if (!strcmp(argv[i], "--greet-only")) cfg.voice_trigger = 0;
        if (!strcmp(argv[i], "--reprov"))   reprov = 1;
        if (!strcmp(argv[i], "--demo-dev")) demo_dev = 1;
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
    }

    const bsp_t *b = bsp_get();
    printf("[板级] %s\n", b->name);
    if (b->init && b->init() != BSP_OK) { fprintf(stderr, "板级初始化失败\n"); return 1; }

    ui_start();                    /* 屏幕不可用也继续跑，只是没画面 */
    ui_set_status("启动中...");

    if (b->audio_start(cfg.sample_rate) != BSP_OK) {
        fprintf(stderr, "音频启动失败\n"); return 1;
    }
    int frame_samples = cfg.sample_rate * cfg.frame_ms / 1000;
    dsp_init(cfg.sample_rate, frame_samples, cfg.aec_tail_ms, cfg.aec_enable);
    ringbuf_init(cfg.sample_rate, 40);

    if (tune) { app_tune(&cfg); return 0; }

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
    };
    if (session_connect(devid, sk, lk, &sui) != 0) return 1;

    mcp_start(session_ctx(), ui_set_status);   /* 注册 take_photo 等设备工具 */

    app_run(&cfg);                             /* 断线才返回 */

    mcp_stop();
    session_close();
    dsp_deinit();
    ui_stop();
    b->audio_stop();
    if (b->display_stop) b->display_stop();
    return 0;
}
