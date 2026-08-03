#ifndef EMOJI_TABLE_H
#define EMOJI_TABLE_H
#include "lvgl.h"
typedef struct { const char *name; const lv_img_dsc_t *img; } emoji_ent_t;
extern const emoji_ent_t EMOJI_TABLE[];
extern const int EMOJI_TABLE_N;
#endif
