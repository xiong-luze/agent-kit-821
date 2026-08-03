/* tai_boot.c —— 从 agentic-kit 的 posix 例程原样抽出的连云引导代码：
 * 最小 JSON 解析 + base64 + session token 解析。两个程序共用，避免手抄出错。
 * 抽自 examples/posix/ai/rtc-tcp-client/audio_chat_demo.c（第 278-439 行）。 */
#include "jsonx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/base64.h"

/* -- Minimal JSON helpers (same as other examples) ---------------------- */

static const char *json_find_value(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return p;
}

int json_get_string(const char *json, const char *key,
                           char *out, size_t cap)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

int json_get_long(const char *json, const char *key, long *out)
{
    const char *p = json_find_value(json, key);
    if (!p) return -1;
    if (*p != '-' && (*p < '0' || *p > '9')) return -1;
    *out = strtol(p, NULL, 10);
    return 0;
}

int json_array_first_string(const char *json, const char *key,
                                   char *out, size_t cap)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '[') return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

char *json_get_object(const char *json, const char *key)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '{') return NULL;
    int depth = 0;
    const char *start = p, *q = p;
    while (*q) {
        if (*q == '{') depth++;
        else if (*q == '}' && --depth == 0) {
            size_t len = (size_t)(q - start + 1);
            char *obj = (char *)malloc(len + 1);
            if (obj) { memcpy(obj, start, len); obj[len] = '\0'; }
            return obj;
        }
        q++;
    }
    return NULL;
}

/* -- Base64 decode (OpenSSL) -------------------------------------------- */

char *b64_decode(const char *encoded, size_t *out_len)
{
    size_t elen = strlen(encoded);
    size_t max_dlen = (elen * 3) / 4 + 4;
    char *out = (char *)malloc(max_dlen + 1);
    if (!out) return NULL;
    size_t dlen = 0;
    if (mbedtls_base64_decode((unsigned char *)out, max_dlen, &dlen,
                               (const unsigned char *)encoded, elen) != 0) {
        free(out);
        return NULL;
    }
    out[dlen] = '\0';
    if (out_len) *out_len = dlen;
    return out;
}

/* 转义感知的字符串提取：处理 \" \\ \/ \n \t \r 和 \uXXXX（含 emoji 用的
 * 代理对），输出 UTF-8。
 * 云端 NLG 的 content 里会带 \u0020 这类转义，朴素提取器会把它原样当成 6 个
 * 字符抄出来（屏幕上就是 "\u0020" 这串乱码）；遇到内嵌的 \" 还会提前截断。 */
static void utf8_put(char *out, size_t cap, size_t *o, unsigned cp)
{
    if (cp < 0x80) {
        if (*o + 1 < cap) out[(*o)++] = (char)cp;
    } else if (cp < 0x800) {
        if (*o + 2 < cap) {
            out[(*o)++] = (char)(0xC0 | (cp >> 6));
            out[(*o)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (*o + 3 < cap) {
            out[(*o)++] = (char)(0xE0 | (cp >> 12));
            out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*o)++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (*o + 4 < cap) {
            out[(*o)++] = (char)(0xF0 | (cp >> 18));
            out[(*o)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[(*o)++] = (char)(0x80 | (cp & 0x3F));
        }
    }
}

static unsigned hex4(const char *p)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if      (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0xFFFFFFFFu;
    }
    return v;
}

int json_get_string_esc(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = json_find_value(json, key);
    if (!p || *p != '"') return -1;
    p++;
    size_t o = 0;
    while (*p && o + 1 < cap) {
        if (*p == '"') break;                    /* 未转义的引号 = 字符串结束 */
        if (*p != '\\') { out[o++] = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': out[o++] = '\n'; p++; break;
        case 't': out[o++] = '\t'; p++; break;
        case 'r': out[o++] = '\r'; p++; break;
        case 'b': out[o++] = '\b'; p++; break;
        case 'f': out[o++] = '\f'; p++; break;
        case 'u': {
            unsigned cp = hex4(p + 1);
            if (cp == 0xFFFFFFFFu) { out[o++] = 'u'; p++; break; }
            p += 5;
            /* 代理对：emoji 等 BMP 外的码点用两个 \uXXXX 表示 */
            if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                unsigned lo = hex4(p + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            utf8_put(out, cap, &o, cp);
            break;
        }
        default:  out[o++] = *p ? *p++ : '\0'; break;   /* \" \\ \/ -> 原字符 */
        }
    }
    out[o] = '\0';
    return 0;
}

/* 就地把文本里**字面量**形式的 \uXXXX 解成真字符（含代理对）。
 * 云端做了双重编码：content 里的 "\\u0020" 经 JSON 解码后得到的是字面的
 * 6 个字符 \ u 0 0 2 0，而不是一个待解的转义。所以在 json_get_string_esc
 * 解完一层之后，还要再扫一遍把这种残留干掉，否则屏幕上就是 \u0020 / \u0022
 * 这类码。 */
void json_unescape_u_inplace(char *s)
{
    if (!s) return;
    size_t o = 0, cap = strlen(s) + 1;
    for (size_t i = 0; s[i]; ) {
        if (s[i] == '\\' && s[i + 1] == 'u') {
            unsigned cp = hex4(s + i + 2);
            if (cp != 0xFFFFFFFFu) {
                size_t adv = 6;
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    s[i + 6] == '\\' && s[i + 7] == 'u') {
                    unsigned lo = hex4(s + i + 8);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        adv = 12;
                    }
                }
                utf8_put(s, cap, &o, cp);
                i += adv;
                continue;
            }
        }
        s[o++] = s[i++];
    }
    s[o] = '\0';
}

