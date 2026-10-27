/*
 * board.c —— 九鼎 V821 aitoy 板：组装 bsp_t 并注册
 *
 * 板子实况见 README §1；音频/屏幕/相机各自的坑分别在
 * audio_alsa.c / display_fb.c / camera_v4l2.c 的文件头里。
 */
#include "bsp/board.h"
#include "board_config.h"
#ifdef HAVE_ALSA_PCM
#include "audio_pcm.h"
#else
#include "audio_alsa.h"
#endif
#include "display_fb.h"
#include "camera_v4l2.h"

#include <stdio.h>
#include <stdlib.h>

static bsp_err_t board_init(void)
{
    /* 音频通路 mixer 配方。amixer cset 不持久，每次启动都得重设。
     * ★ 逐条检查退出码：设不上要说出来。原来是拼好的命令串直接 system()、
     * 返回值没人看，numid 不存在也毫无痕迹（见 board_config.h 的注释）。 */
    static const board_mixer_t mixer[] = BOARD_MIXER_SETUP;
    int failed = 0;
    for (int i = 0; mixer[i].desc; i++) {
        char cmd[96];
        snprintf(cmd, sizeof cmd,
                 "amixer -c 0 cset numid=%d %d >/dev/null 2>&1",
                 mixer[i].numid, mixer[i].value);
        if (system(cmd) != 0) {
            fprintf(stderr, "[板级] ✗ mixer numid=%d 设为 %d 失败 —— %s\n",
                    mixer[i].numid, mixer[i].value, mixer[i].desc);
            failed++;
        }
    }
    if (failed)
        fprintf(stderr, "[板级] ⚠ %d 条 mixer 设定没生效，音频可能不正常"
                        "（用 `amixer -c 0 contents` 看这套固件实际有哪些控件）\n", failed);
    else
        printf("[板级] v821_aitoy 音频通路已配置\n");
    return BSP_OK;
}

static bsp_err_t cam_start(int w, int h) { return cam_open(BOARD_CAM_DEV, w, h); }

static const bsp_t s_bsp = {
    .name = "v821_aitoy",
    .caps = BSP_CAP_AUDIO | BSP_CAP_DISPLAY | BSP_CAP_CAMERA
#ifdef HAVE_ALSA_PCM
            | BSP_CAP_AUDIO_DUPLEX
#endif
            ,

    .init = board_init,

    .audio_start   = board_audio_start,
    .audio_read    = board_audio_read,
    .audio_write   = board_audio_write,
    .audio_mic_set = board_audio_mic_set,
    .audio_drain   = board_audio_drain,
    .audio_stop    = board_audio_stop,
#ifdef HAVE_ALSA_PCM
    .audio_flush   = board_audio_flush,
    .audio_pending = board_audio_pending,
#endif

    .display_start = board_display_start,
    .display_flush = board_display_flush,
    .display_stop  = board_display_stop,

    .camera_start        = cam_start,
    .camera_capture_jpeg = cam_capture_jpeg,
    .camera_capture_rgb  = cam_capture_rgb,
    .camera_get_size     = cam_get_size,
    .camera_stop         = cam_close,
};

DECLARE_BOARD(&s_bsp);
