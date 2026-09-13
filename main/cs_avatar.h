// cs_avatar.h -- NiKo 头像(真实位图,编入固件的 104/52 两档 RGB565)。
#pragma once

#include "lvgl.h"

// 头像原图由 tools 侧脚本生成:assets 插画 -> 裁剪 -> 104/52 两档 RGB565,
// 输出到 main/cs_avatar_data.c。取 image 后可直接 lv_image_set_src;
// 需要缩放时用 lv_image_set_scale(256 = 1x)。
const lv_image_dsc_t *cs_avatar_image(int size);

// 在 parent 的 (x,y) 处画一个 size×size 的头像牌:战队色描边 + 圆角 + 位图。
// size <= 72 用 52px 原图,否则用 104px 原图(避免放大糊掉)。
void cs_avatar_draw(struct _lv_obj_t *parent, int x, int y, int size);
