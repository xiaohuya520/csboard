// cs_avatar.h -- NiKo 头像(程序化绘制,无二进制依赖)。
#pragma once

#include "lvgl.h"

// 在 parent 的 (x,y) 处绘制一个 size×size 的圆形选手徽章:
// 战队色外环 + 深色内圆 + 准星 + 选手名。必编译、必显示。
// 想换成真实照片:把 RGB565 像素数组用 lv_image_dsc_t 包好,在调用处替换本函数即可
// (API 与 cs_logos 的 cs_logo_get 一致,可直接 lv_image_set_src)。
struct _lv_obj_t;
void cs_avatar_draw(struct _lv_obj_t *parent, int x, int y, int size);
