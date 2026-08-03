/* emoji_table.c —— 情绪名 → 32x32 图。情绪名来自云端 SKILL 包的
 * data.skillContent.emotion[0]，词表与官方 xiaozhi 一致。 */
#include "lvgl.h"
#include "emoji_table.h"

extern const lv_img_dsc_t emoji_1f602;
extern const lv_img_dsc_t emoji_1f606;
extern const lv_img_dsc_t emoji_1f609;
extern const lv_img_dsc_t emoji_1f60c;
extern const lv_img_dsc_t emoji_1f60d;
extern const lv_img_dsc_t emoji_1f60e;
extern const lv_img_dsc_t emoji_1f60f;
extern const lv_img_dsc_t emoji_1f614;
extern const lv_img_dsc_t emoji_1f618;
extern const lv_img_dsc_t emoji_1f61c;
extern const lv_img_dsc_t emoji_1f620;
extern const lv_img_dsc_t emoji_1f62d;
extern const lv_img_dsc_t emoji_1f62f;
extern const lv_img_dsc_t emoji_1f631;
extern const lv_img_dsc_t emoji_1f633;
extern const lv_img_dsc_t emoji_1f634;
extern const lv_img_dsc_t emoji_1f636;
extern const lv_img_dsc_t emoji_1f642;
extern const lv_img_dsc_t emoji_1f644;
extern const lv_img_dsc_t emoji_1f914;
extern const lv_img_dsc_t emoji_1f924;

const emoji_ent_t EMOJI_TABLE[] = {
    { "FUNNY", &emoji_1f602 },
    { "LAUGHING", &emoji_1f606 },
    { "WINKING", &emoji_1f609 },
    { "RELAXED", &emoji_1f60c },
    { "LOVING", &emoji_1f60d },
    { "COOL", &emoji_1f60e },
    { "CONFIDENT", &emoji_1f60f },
    { "SAD", &emoji_1f614 },
    { "KISSY", &emoji_1f618 },
    { "SILLY", &emoji_1f61c },
    { "ANGRY", &emoji_1f620 },
    { "CRYING", &emoji_1f62d },
    { "SURPRISED", &emoji_1f62f },
    { "SHOCKED", &emoji_1f631 },
    { "EMBARRASSED", &emoji_1f633 },
    { "SLEEPY", &emoji_1f634 },
    { "NEUTRAL", &emoji_1f636 },
    { "HAPPY", &emoji_1f642 },
    { "CONFUSED", &emoji_1f644 },
    { "THINKING", &emoji_1f914 },
    { "DELICIOUS", &emoji_1f924 },
};
const int EMOJI_TABLE_N = sizeof(EMOJI_TABLE)/sizeof(EMOJI_TABLE[0]);
