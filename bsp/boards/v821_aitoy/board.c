/*
 * board.c —— 九鼎 V821 aitoy 板：组装 bsp_t 并注册
 *
 * 板子实况见 README §1；音频/屏幕/相机各自的坑分别在
 * audio_alsa.c / display_fb.c / camera_v4l2.c 的文件头里。
 */
#include "bsp/board.h"
#include "board_config.h"
#include "audio_alsa.h"
#include "display_fb.h"
#include "camera_v4l2.h"

#include <stdio.h>
#include <stdlib.h>

static bsp_err_t board_init(void)
{
    /* 音频通路 mixer 配方。amixer cset 不持久，每次启动都得设。 */
    static const char *mixer[] = BOARD_MIXER_SETUP;
    for (int i = 0; mixer[i]; i++) system(mixer[i]);
    printf("[板级] %s 音频通路已配置\n", "v821_aitoy");
    return BSP_OK;
}

static bsp_err_t cam_start(int w, int h) { return cam_open(BOARD_CAM_DEV, w, h); }

static const bsp_t s_bsp = {
    .name = "v821_aitoy",
    .caps = BSP_CAP_AUDIO | BSP_CAP_DISPLAY | BSP_CAP_CAMERA,

    .init = board_init,

    .audio_start   = board_audio_start,
    .audio_read    = board_audio_read,
    .audio_write   = board_audio_write,
    .audio_mic_set = board_audio_mic_set,
    .audio_drain   = board_audio_drain,
    .audio_stop    = board_audio_stop,

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
