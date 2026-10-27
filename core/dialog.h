/* Pure control logic; no audio device, cloud, UI, or keyword engine dependency. */
#ifndef DIALOG_H
#define DIALOG_H
#include <stdint.h>

typedef enum { DIALOG_SLEEP, DIALOG_LISTEN, DIALOG_SEND, DIALOG_WAIT, DIALOG_PLAY } dialog_state_t;
enum {
    DIALOG_START = 1u << 0, DIALOG_AUDIO = 1u << 1, DIALOG_END = 1u << 2,
    DIALOG_INTERRUPT = 1u << 3, DIALOG_WOKE = 1u << 4,
    DIALOG_FINISHED = 1u << 5, DIALOG_SLEPT = 1u << 6
};
typedef struct {
    int frame_ms, wake_enabled, wake_hold_ms;
    int vad_start, vad_end, vad_hang_ms, barge_ms;
    int max_turn_ms, reply_timeout_ms, tail_quiet_ms;
} dialog_config_t;
typedef struct {
    int64_t now_ms, reply_quiet_ms;
    int rms, wake, asr_final, reply_audio, reply_done, playback_active, echo_likely;
} dialog_input_t;
typedef struct {
    dialog_config_t cfg;
    dialog_state_t state;
    int voiced, silent;
    int64_t turn_start_ms, wait_start_ms, awake_until_ms;
} dialog_t;

int dialog_init(dialog_t *dialog, const dialog_config_t *config, int64_t now_ms);
/* START means send the pre-roll including the current frame, not AUDIO twice.
 * INTERRUPT must flush local playback before posting network commands. */
unsigned dialog_step(dialog_t *dialog, const dialog_input_t *input);
#endif
