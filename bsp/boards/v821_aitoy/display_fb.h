/* display_fb.h —— 板内私有接口 */
#ifndef BOARD_DISPLAY_FB_H
#define BOARD_DISPLAY_FB_H
int  board_display_start(int *w, int *h);
void board_display_flush(const void *rgb565, int x, int y, int w, int h);
void board_display_stop(void);
#endif
