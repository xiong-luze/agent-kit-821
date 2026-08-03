/*
 * text.h —— 文本处理小工具
 */
#ifndef TEXT_H
#define TEXT_H
#include <stdint.h>
#include <stddef.h>

/* 剥掉 emoji：中文字体没有这些字形，留着只会渲染成豆腐块。
 * 情绪改由界面上那张 32x32 图表现。first_cp 回传首个 emoji 码点（可为 NULL）。 */
void text_strip_emoji(const char *in, char *out, size_t cap, uint32_t *first_cp);
#endif
