// cs_avatar.c —— NiKo 头像(程序化绘制)
//
// 设计取舍:真实选手照片需要二进制图片资源(几十 KB 的 RGB565 数组),会增大固件
// 且涉及版权。看板面向粉丝,用「战队色圆环 + 准星 + 选手名」的程序化徽章更稳妥:
// 零资源、必然可编译、必然可显示,且一眼能认出是「NiKo 的看板」。
// 需要真实照片时,按 cs_avatar.h 顶部说明替换即可。
#include "cs_avatar.h"
#include "cs_data.h"
#include "cs_fonts.h"

#include "lvgl.h"
#include <string.h>

// 取一个尺寸内的居中矩形(去掉滚动标志),统一封装避免重复
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     uint32_t bg, uint32_t border)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, w / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, border ? 2 : 0, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(border ? border : bg), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

void cs_avatar_draw(lv_obj_t *parent, int x, int y, int size)
{
    const cs_niko_t *n = cs_niko();
    uint32_t ring = (n && n->team_color) ? n->team_color : 0xE43B2F;

    // 外环(战队色)
    lv_obj_t *o = box(parent, x, y, size, size, ring, 0);

    // 内圆(深色底)
    int in = size * 74 / 100;
    lv_obj_t *inner = box(o, (size - in) / 2, (size - in) / 2, in, in, 0x0E1116, 0);

    // 准星:横 + 竖 两条细线(电竞风)
    int cl = in * 46 / 100;
    lv_obj_t *h = lv_obj_create(inner);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(h, (in - cl) / 2, in / 2 - 1);
    lv_obj_set_size(h, cl, 2);
    lv_obj_set_style_bg_color(h, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(h, 0, 0);
    lv_obj_set_style_pad_all(h, 0, 0);

    lv_obj_t *v = lv_obj_create(inner);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(v, in / 2 - 1, (in - cl) / 2);
    lv_obj_set_size(v, 2, cl);
    lv_obj_set_style_bg_color(v, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(v, 0, 0);
    lv_obj_set_style_pad_all(v, 0, 0);

    // 选手名
    lv_obj_t *nm = lv_label_create(inner);
    lv_obj_set_style_text_font(nm, &font_cn16, 0);
    lv_obj_set_style_text_color(nm, lv_color_hex(0xE6EDF3), 0);
    lv_label_set_text(nm, (n && n->name[0]) ? n->name : "NiKo");
    lv_obj_set_width(nm, in);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nm, LV_ALIGN_CENTER, 0, in * 20 / 100);
}
