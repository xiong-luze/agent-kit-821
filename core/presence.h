/*
 * presence.h —— 有人靠近检测（本地视觉，不上云）
 *
 * 用途：待机久了没人说话时，看到有人走过来就主动打个招呼。
 *
 * ★ 判据是「窗口内命中率」而不是「连续 N 帧都检到」：喂图没处理好的时候单帧
 * 结果会在 1/0 之间乱跳（实测最差 37%），连续条件几乎永远不成立。
 * 现在预处理修好了（面积平均 + letterbox，实测有人约 100%、空场景约 9%），
 * 但仍保留命中率形式——真人会动会侧身，不能假设单帧永远可靠。
 * 详见 presence.c 文件头。
 *
 * 跑在自己的线程里，靠 bsp_get()->camera_capture_rgb() 取帧，用全志官方预编译的
 * riscv32 人形检测库（third_party/pdet，纯 CPU 软推理，这颗芯片没有 NPU）。
 * 单帧约 167ms @ 320x192。检测和音频循环抢同一个 1GHz 标量核，所以：
 *   - 对话期间必须 presence_set_enabled(0) 彻底停掉
 *   - 线程自己降 nice，让音频线程随时能抢占
 */
#ifndef PRESENCE_H
#define PRESENCE_H

typedef struct {
    /* --- 两级采样（见 presence.c 文件头）--- */
    int   watch_ms;        /* 巡视期采样周期：多久抓一帧看一眼 */
    int   confirm_ms;      /* 确认期时长：连着抓帧统计命中率 */
    float confirm_ratio;   /* 确认期内命中率达到多少算真的有人（实测 0.6 合适） */

    /* --- 防重复骚扰 --- */
    int   rearm_quiet_ms;  /* 连续没人多久才重新武装 */
    int   min_gap_ms;      /* 两次主动打招呼的最小间隔，兜底 */

    /* --- 检测器 --- */
    int   det_w, det_h;    /* 模型输入，固定 320x192 */
    float threshold;       /* 置信度门限，官方默认 0.3 */
    int   letterbox;       /* 1 = 保持宽高比加灰边；0 = 直接拉伸 */
} presence_cfg_t;

void presence_cfg_defaults(presence_cfg_t *c);

/* 起检测线程。失败返回非 0 —— **调用方应当继续运行**，只是没有这个功能
 * （没编进检测库、相机不可用、内存不够都会失败）。 */
int  presence_start(const presence_cfg_t *c);
void presence_stop(void);

/* 开/关检测。关掉后线程只睡不干活，CPU 归零。纯粹的开关，没有副作用 —— 因为
 * 环境噪声会触发一堆空对话，每次都跟着开关一遍，不能让开关本身影响判断状态。 */
void presence_set_enabled(int on);

/* 强制重新武装：要求先满足 rearm_quiet_ms 的无人才可能再触发。
 * **只在真的聊过一轮之后调**（云端确实回了话）—— 刚聊完的人就站在镜头前，
 * 不该等 60 秒后又被搭话一次。噪声触发的空对话不要调它。 */
void presence_rearm(void);

/* 取走一次触发（消费即清）。0 = 没有。主循环轮询它。 */
int  presence_take_trigger(void);

/* 只做检测并打印耗时/命中率，不进状态机。--pdet-tune 用，阻塞到 Ctrl+C。
 * 输出的「命中率」就是 confirm_ratio 该怎么设的依据：
 * 分别量「有人站着」和「空场景」两个数，中间取一刀。 */
void presence_tune(const presence_cfg_t *c);

#endif /* PRESENCE_H */
