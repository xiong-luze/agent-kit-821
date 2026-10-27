/*
 * audio_alsa.c —— V821 aitoy 板的音频 I/O（arecord / aplay 管道）
 *
 * 为什么用管道而不是 alsa-lib：这两个工具在本板已验证可用，零新依赖，
 * 产物只依赖 libc.so。将来要上官方硬件 AEC（MPP 的 AO/AI）时，只需换掉
 * 这个文件，core/ 和 app/ 一行不用改 —— 这正是 bsp 层存在的意义。
 *
 * ★ 两个非直觉的地方，都是实测踩出来的：
 * 1) aplay 必须**每帧都写**，不播时也要喂静音帧。只在有内容时写会让它断流
 *    underrun，时钟一乱后面全乱。
 * 2) "播完"只能靠 pclose 判定。我们和 aplay 之间隔着 64KB 的 OS 管道缓冲
 *    （约 2 秒音频），fwrite 只要管道没满就立即返回，"写完"远早于"播完"。
 *    pclose 会阻塞到 aplay 把管道和内部缓冲全部放完、进程退出。
 */
#include "audio_alsa.h"
#include "board_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static FILE *g_rec  = NULL;      /* NULL = 麦克风已关 */
static FILE *g_play = NULL;
static int   g_frame_bytes = 640;   /* 20ms @16k mono 16bit */
static int   g_play_err_warned = 0; /* 播放出错只报一次，别在 20ms 循环里刷屏 */

/* ★ popen() 只要 fork 成功就返回非 NULL —— aplay 自己打不开声卡它完全不知道，
 * 报错还是异步打到 stderr 的。症状是「文字一切正常，就是没有声音」，而且没有任何
 * 一行明确的错误，非常难查（踩过：上一次的进程没退，占着 playback，
 * aplay 报 "audio open error: Resource busy" 混在几百行云端日志里）。
 * 所以先用一帧静音探一次，把失败变成启动时就能看见的硬错误。 */
static int probe_playback(void)
{
    int rc = system(BOARD_APLAY_PROBE);
    if (rc == 0) return 0;
    fprintf(stderr,
        "[音频] ✗ 播放设备打不开（aplay 探测失败，rc=%d）\n"
        "       最常见原因：**上一次的进程还在跑，占着声卡**。\n"
        "       adb 断开/重启不会杀掉板上的进程，用这条清一下：\n"
        "         killall v821_chatbot aplay arecord; pgrep -f '[v]821_chatbot|[a]play|[a]record|[x]star'\n"
        "       其次是官方 xstar 被 daemon.sh 拉起来了（先 kill daemon.sh 再 killall xstar）。\n",
        rc);
    return -1;
}

int board_audio_start(int sample_rate)
{
    (void)sample_rate;              /* 命令行里写死 16k，与上层约定一致 */
    if (probe_playback() != 0) return -1;

    g_play = popen(BOARD_APLAY_CMD, "w");
    if (!g_play) { fprintf(stderr, "[音频] aplay 启动失败\n"); return -1; }
    g_rec = popen(BOARD_AREC_CMD, "r");
    if (!g_rec) { fprintf(stderr, "[音频] arecord 启动失败\n"); return -1; }
    return 0;
}

void board_audio_stop(void)
{
    if (g_rec)  { pclose(g_rec);  g_rec  = NULL; }
    if (g_play) { pclose(g_play); g_play = NULL; }
}

int board_audio_read(int16_t *mono, int frames)
{
    if (!g_rec) return 0;                       /* 麦克风关着 */
    size_t want = (size_t)frames * sizeof(int16_t);
    g_frame_bytes = (int)want;
    if (fread(mono, 1, want, g_rec) != want) return -1;
    return frames;
}

int board_audio_write(const int16_t *mono, int frames)
{
    if (!g_play) return -1;
    size_t want = (size_t)frames * sizeof(int16_t);
    fwrite(mono, 1, want, g_play);
    /* aplay 中途死掉（声卡被抢、underrun 退出）也要说一声，否则又是静默无声。
     * 只报一次，主循环每 20ms 来一趟，刷屏会把真正的日志埋掉。 */
    if (ferror(g_play) && !g_play_err_warned) {
        g_play_err_warned = 1;
        fprintf(stderr, "[音频] ✗ 播放管道出错（aplay 可能已退出），之后不会有声音\n");
    }
    return frames;
}

void board_audio_mic_set(int on)
{
    if (on && !g_rec) {
        g_rec = popen(BOARD_AREC_CMD, "r");
        if (!g_rec) fprintf(stderr, "[音频] arecord 重开失败\n");
    } else if (!on && g_rec) {
        pclose(g_rec);
        g_rec = NULL;
    }
}

void board_audio_drain(void)
{
    if (!g_play) return;
    pclose(g_play);                 /* 阻塞到真正播完、aplay 退出 */
    g_play = popen(BOARD_APLAY_CMD, "w");
    if (!g_play) fprintf(stderr, "[音频] aplay 重开失败\n");
}
