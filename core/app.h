/*
 * app.h —— 对话状态机
 *
 * 平台无关：音频进出走 bsp，云端收发走 service/session，界面走 ui。
 * 形态是**多轮一问一答**：聆听 → 上送 → 播放（期间物理关麦）→ 回到聆听。
 */
#ifndef APP_H
#define APP_H

typedef struct {
    int sample_rate;
    int frame_ms;
    int vad_start;      /* 0 = 开机自动标定 */
    int vad_end;        /* 0 = 开机自动标定 */
    int vad_hang_ms;    /* 静音多久算说完 */
    int guard_ms;       /* 播完后再忽略麦克风多久（覆盖房间混响） */
    int aec_tail_ms;
    int aec_enable;
    int full_duplex;    /* Requires direct ALSA backend and working AEC. */
    int wake_enabled;
    int wake_min_rms;   /* Raw-input activity floor; match still requires KWS. 0 disables. */
    const char *wake_word, *wake_model, *wake_dictionary;
    double wake_threshold;
    int wake_hold_ms, barge_ms, pre_roll_ms;

    /* 主动打招呼：待机超过 greet_idle_ms 又没人说话，摄像头确认有人靠近就
     * 自己开口。0 = 关。检测参数见 core/presence.h。 */
    int greet_enable;
    int greet_idle_ms;

    /* 0 = 不用声音触发对话，只留主动打招呼那条路。
     * 用途：能量 VAD 分不出"你说话"和"旁边同事说话"，在办公室这种环境里噪声会
     * 一直触发假对话、把待机窗口搅得没法验证 presence。关掉它就能单独验打招呼。
     * 这不是正常形态，只是测试开关（--greet-only）。 */
    int voice_trigger;
} app_cfg_t;

void app_cfg_defaults(app_cfg_t *c);   /* 填默认值，再读环境变量覆盖 */
int app_cfg_validate(const app_cfg_t *c);
/* Connection/uplink faults and explicit audio discontinuities are retryable.
 * Other device/model failures remain terminal. */
enum { APP_DUPLEX_ERROR = -1, APP_DUPLEX_RECONNECT = -2, APP_DUPLEX_REBUILD_AUDIO = -3 };
int app_run_duplex(const app_cfg_t *c);

/* 只量麦克风电平并打印，用来定 VAD 阈值。阻塞到 Ctrl+C。 */
void app_tune(const app_cfg_t *c);

/* 跑对话主循环。连接断开才返回。 */
void app_run(const app_cfg_t *c);

#endif /* APP_H */
