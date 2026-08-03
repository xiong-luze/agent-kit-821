# 界面资源的来源与许可

本目录下的字体和表情**不是本项目原创**，是从第三方素材转换来的。开源分发需要写清来源，
所以单独记在这里。

## fonts/font_puhui_14_1.c

用 LVGL 官方的 `lv_font_conv` 从 **阿里巴巴普惠体 3.0**
（`AlibabaPuHuiTi-3-55-Regular.ttf`）生成的位图字体，14px / 1bpp，字表覆盖 ASCII +
常用汉字 + 常用符号。生成参数保留在文件头部的注释里。

- 原字体：阿里巴巴普惠体（Alibaba PuHuiTi），阿里巴巴集团发布，**免费商用授权**
- 转换工具：`lv_font_conv`（LVGL，MIT）
- 生成物是原字体的派生作品，使用时请遵守阿里巴巴普惠体的授权条款

## emoji/*.c

23 个 LVGL 图片资源，由 **xiaozhi 项目的 32bpp 表情素材**转换而来
（ARGB8888 → RGB565+A8，因为本板 `LV_COLOR_DEPTH=16`）。转换脚本是
`tools/gen_emoji_16bpp.py`。

> ⚠️ **出处待核实**：这些表情的**最初**来源（xiaozhi 那边素材本身取自哪套 emoji）
> 我们没有查到记录。常见的开源 emoji 集各有要求，例如 Twemoji 是 CC-BY 4.0
> （**需要署名**）、Noto Emoji 是 OFL。
>
> 这里如实说明现状，并**欢迎认出出处的人提 issue 告知**，我们会补上相应署名。
> 如果你要把本仓库用于正式产品，请自行核实后补齐署名，或换成一套你已确认许可的
> 表情 —— 只需替换 `emoji/` 下的资源并调整 `ui/ui.c` 里的映射表，其余代码不用动。

## lv_conf.h / lv_drv_conf.h

从 LVGL 8.3.2 自带的模板改的，跟随 **LVGL 的 MIT 许可**。
只改了配置项（`LV_COLOR_DEPTH=16`、字体、内存池等），没有改代码。

## 本项目自身

`LICENSE`（Apache-2.0），与上游 [tuya/agentic-kit](https://github.com/tuya/agentic-kit) 一致。
