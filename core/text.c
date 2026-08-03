/* text.c —— 见 text.h */
#include "text.h"
#include <string.h>

void text_strip_emoji(const char *in, char *out, size_t cap, uint32_t *first_cp)
{
    size_t o = 0;
    if (first_cp) *first_cp = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < cap; ) {
        int n = 1; uint32_t cp = *p;
        if      ((*p & 0x80) == 0)    { n = 1; cp = *p; }
        else if ((*p & 0xE0) == 0xC0) { n = 2; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { n = 3; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { n = 4; cp = *p & 0x07; }
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { n = 1; cp = *p; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        int is_emoji = (cp >= 0x1F000 && cp <= 0x1FAFF) ||
                       (cp >= 0x2600  && cp <= 0x27BF)  ||
                       cp == 0xFE0F || cp == 0x200D ||
                       (cp >= 0x2190 && cp <= 0x21FF);
        if (is_emoji) {
            if (first_cp && !*first_cp) *first_cp = cp;
        } else if (o + (size_t)n < cap) {
            memcpy(out + o, p, (size_t)n); o += (size_t)n;
        }
        p += n;
    }
    /* 去掉前导空白（剥掉 emoji 后常留一个空格） */
    size_t st = 0;
    while (st < o && (out[st] == ' ' || out[st] == '\n')) st++;
    if (st) memmove(out, out + st, o - st), o -= st;
    out[o] = '\0';
}

