#!/usr/bin/env python3
"""把官方 xiaozhi 的 32bpp emoji 素材转成本机用的 RGB565+A8，并生成情绪名映射表。

为什么要转：官方素材（chatbox/xiaozhi/port/lvgl/fonts/src/emoji/）是**只按
LV_COLOR_DEPTH=32 生成**的（每张 32x32 图 4096 字节 = 32*32*4），源文件里没有
`#if LV_COLOR_DEPTH` 分支。而本板 framebuffer 是 RGB565 16bpp，LVGL 也按 16
编译，直接用会颜色错乱、并且按 3 字节/像素读取时越界。
转换后每张 3072 字节（RGB565 2B + alpha 1B），总体积 592K -> 432K。

映射用**情绪名**而不是 Unicode 码点：云端 SKILL 包给的是
data.skillContent.emotion[0]（HAPPY / NEUTRAL / THINKING ...），而官方那 21 张图
在 font_emoji_32.c 里的语义注释正好是同一套词表，按名字匹配比按码点稳。

用法（在仓库根目录）：
    python3 tools/gen_emoji_16bpp.py            # 输出到 src/emoji/
"""
import re, os, glob, sys

SRC = os.path.expanduser(
    '~/kit/v821-tina-v12/platform/allwinner/llm/chatbox/xiaozhi/'
    'port/lvgl/fonts/src/emoji')
DST = 'src/emoji'

# 取自官方 font_emoji_32.c 里每个 extern 后面的语义注释
NAME = {
    '1f636': 'NEUTRAL',     '1f642': 'HAPPY',       '1f606': 'LAUGHING',
    '1f602': 'FUNNY',       '1f614': 'SAD',         '1f620': 'ANGRY',
    '1f62d': 'CRYING',      '1f60d': 'LOVING',      '1f633': 'EMBARRASSED',
    '1f62f': 'SURPRISED',   '1f631': 'SHOCKED',     '1f914': 'THINKING',
    '1f609': 'WINKING',     '1f60e': 'COOL',        '1f60c': 'RELAXED',
    '1f924': 'DELICIOUS',   '1f618': 'KISSY',       '1f60f': 'CONFIDENT',
    '1f634': 'SLEEPY',      '1f61c': 'SILLY',       '1f644': 'CONFUSED',
}


def main():
    if not os.path.isdir(SRC):
        sys.exit('找不到官方素材目录：%s\n（需要先解压九鼎 SDK）' % SRC)
    os.makedirs(DST, exist_ok=True)

    made = []
    for f in sorted(glob.glob(SRC + '/*.c')):
        code = re.search(r'emoji_([0-9a-f]+)_32', os.path.basename(f)).group(1)
        body = re.search(r'\{([^}]*0x[0-9a-fA-F]{2}[^}]*)\}',
                         open(f).read(), re.S)
        by = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body.group(1))]
        if len(by) != 32 * 32 * 4:
            print('跳过 %s（%d 字节，不是 32bpp 的 32x32）' % (code, len(by)))
            continue

        # LVGL 32bpp TRUE_COLOR_ALPHA 内存序是 B,G,R,A
        out = []
        for i in range(32 * 32):
            b, g, r, a = by[i * 4: i * 4 + 4]
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out += [v & 0xFF, (v >> 8) & 0xFF, a]      # RGB565 小端 + alpha

        var = 'emoji_%s' % code
        L = ['/* 由官方 32bpp 素材转换：ARGB8888 -> RGB565+A8（本机 LV_COLOR_DEPTH=16）',
             ' * 生成脚本 tools/gen_emoji_16bpp.py，勿手改。 */',
             '#include "lvgl.h"', '',
             'static const uint8_t %s_map[] = {' % var]
        for i in range(0, len(out), 16):
            L.append('    ' + ','.join('0x%02x' % c for c in out[i:i + 16]) + ',')
        L += ['};', '',
              'const lv_img_dsc_t %s = {' % var,
              '    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,',
              '    .header.always_zero = 0, .header.reserved = 0,',
              '    .header.w = 32, .header.h = 32,',
              '    .data_size = sizeof(%s_map), .data = %s_map,' % (var, var),
              '};']
        open('%s/%s.c' % (DST, var), 'w').write('\n'.join(L) + '\n')
        made.append((code, NAME.get(code, '?'), var))

    T = ['/* emoji_table.c —— 情绪名 -> 32x32 图。情绪名来自云端 SKILL 包的',
         ' * data.skillContent.emotion[0]。生成脚本 tools/gen_emoji_16bpp.py。 */',
         '#include "lvgl.h"', '#include "emoji_table.h"', '']
    T += ['extern const lv_img_dsc_t %s;' % v for _, _, v in made]
    T += ['', 'const emoji_ent_t EMOJI_TABLE[] = {']
    T += ['    { "%s", &%s },' % (n, v) for _, n, v in made if n != '?']
    T += ['};',
          'const int EMOJI_TABLE_N = sizeof(EMOJI_TABLE)/sizeof(EMOJI_TABLE[0]);']
    open('%s/emoji_table.c' % DST, 'w').write('\n'.join(T) + '\n')

    open('%s/emoji_table.h' % DST, 'w').write(
        '#ifndef EMOJI_TABLE_H\n#define EMOJI_TABLE_H\n#include "lvgl.h"\n'
        'typedef struct { const char *name; const lv_img_dsc_t *img; } emoji_ent_t;\n'
        'extern const emoji_ent_t EMOJI_TABLE[];\nextern const int EMOJI_TABLE_N;\n'
        '#endif\n')

    print('转换 %d 张，输出到 %s/' % (len(made), DST))


if __name__ == '__main__':
    main()
