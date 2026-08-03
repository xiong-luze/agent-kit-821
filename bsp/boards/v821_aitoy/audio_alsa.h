/* audio_alsa.h —— 板内私有接口，只给 board.c 组装 vtable 用 */
#ifndef BOARD_AUDIO_ALSA_H
#define BOARD_AUDIO_ALSA_H
#include <stdint.h>
int  board_audio_start(int sample_rate);
void board_audio_stop(void);
int  board_audio_read(int16_t *mono, int frames);
int  board_audio_write(const int16_t *mono, int frames);
void board_audio_mic_set(int on);
void board_audio_drain(void);
#endif
