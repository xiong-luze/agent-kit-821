/* Board-private direct ALSA backend; selected instead of audio_alsa.c. */
#ifndef BOARD_AUDIO_PCM_H
#define BOARD_AUDIO_PCM_H

#include <stdint.h>

/* All calls belong to the same audio thread. Only 16 kHz mono S16_LE is accepted.
 * V821_ALSA_DEVICE optionally overrides hw:0,0. No mixer settings are changed. */
int  board_audio_start(int sample_rate);
void board_audio_stop(void);
/* Exact requested frame count, zero when capture is paused, or a negative error.
 * XRUN/suspend maps to BSP_ERR_AUDIO_DISCONTINUITY in bsp/board.h.
 * Transfers and legacy drain wait no more than two seconds. An XRUN/error is
 * sticky until stop/start: no unreported reference/capture discontinuities. */
int  board_audio_read(int16_t *mono, int frames);
int  board_audio_write(const int16_t *mono, int frames);
/* Legacy half-duplex compatibility. Full-duplex code keeps capture enabled.
 * A failure in either void interface is exposed by the next int operation. */
void board_audio_mic_set(int on);
void board_audio_drain(void);
/* Drop queued playback immediately, then prepare/prime/start only playback,
 * leaving one frame free so a write in the same loop cannot delay capture.
 * Capture remains running. Caller must reset/reconcile its AEC reference history
 * because discarded samples will never reach the DAC. Returns 0 or a negative error. */
int  board_audio_flush(void);
/* Queued DAC samples, INCLUDING silence, or a negative error. Continuous silence
 * keeps this positive; caller must track when its last speech sample will drain. */
int  board_audio_pending(void);

#endif
