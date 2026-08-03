/*
 * jsonx.h —— 最小 JSON 取值（够用就好，不引入 cJSON）
 *
 * 云端文本要过两层解码，否则屏幕上是 \u0020 / \u0022 这类码：
 *   ① json_get_string_esc()   解 JSON 转义（\uXXXX、代理对、内嵌 \"）
 *   ② json_unescape_u_inplace() 再解**字面量**残留 —— 云端做了双重编码，
 *      \\u0020 解完一层 JSON 得到的是字面的 6 个字符，不是转义。
 * 朴素提取器除了漏这两层，遇到内嵌引号还会提前截断。
 */
#ifndef JSONX_H
#define JSONX_H

#include <stdint.h>
#include <stddef.h>

int   json_get_string(const char *json, const char *key, char *out, size_t cap);
int   json_get_long(const char *json, const char *key, long *out);
char *json_get_object(const char *json, const char *key);
int   json_array_first_string(const char *json, const char *key,
                              char *out, size_t cap);

/* 转义感知版：取云端 NLG 的 content、ASR 的 text 必须用它 */
int   json_get_string_esc(const char *json, const char *key, char *out, size_t cap);
/* 就地解掉字面量形式的 \uXXXX（云端双重编码的残留），在上面那个之后调 */
void  json_unescape_u_inplace(char *s);

/* base64 解码（session token 是 base64 包着的 JSON） */
char *b64_decode(const char *encoded, size_t *out_len);

#endif /* JSONX_H */
