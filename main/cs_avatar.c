// cs_avatar.c —— NiKo 头像:把生成好的 104/52 两档 RGB565 位图包成 LVGL 描述符。
//
// 为什么改成真图:程序化画的「战队色圆环 + 准星」虽然必编译必显示,但一眼看不出是谁。
// 现在的位图是静态资源(约 27KB),编进 app 里,不依赖任何运行期解码器,
// 也不占堆内存 —— 只在需要时给 LVGL 一个描述符。
#include "cs_avatar.h"
#include "cs_data.h"

#include <string.h>

extern const uint16_t cs_avatar104[104 * 104];
extern const uint16_t cs_avatar52[52 * 52];

static lv_image_dsc_t s_d104;
static lv_image_dsc_t s_d52;
static bool           s_ready;

static void make_dsc(lv_image_dsc_t *dsc, const uint16_t *pix, int edge)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = (uint32_t)edge;
    dsc->header.h      = (uint32_t)edge;
    dsc->header.stride = (uint32_t)edge * 2u;
    dsc->data_size     = (uint32_t)edge * edge * 2u;
    dsc->data          = (const uint8_t *)pix;
}

static bool ensure(void)
{
    if (s_ready) return true;
    make_dsc(&s_d104, cs_avatar104, 104);
    make_dsc(&s_d52,  cs_avatar52,  52);
    s_ready = true;
    return true;
}

const lv_image_dsc_t *cs_avatar_image(int size)
{
    ensure();
    return (size <= 72) ? &s_d52 : &s_d104;
}

void cs_avatar_draw(lv_obj_t *parent, int x, int y, int size)
{
    if (!parent || size <= 0) return;
    ensure();

    const cs_niko_t *n = cs_niko();
    uint32_t ring = (n && n->team_color) ? n->team_color : 0x00B36B;
    int native = (size <= 72) ? 52 : 104;

    // 外框:战队色描边 + 深色底,把头像框成一张「选手牌」
    lv_obj_t *f = lv_obj_create(parent);
    lv_obj_remove_flag(f, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(f, x, y);
    lv_obj_set_size(f, size, size);
    lv_obj_set_style_radius(f, size / 6, 0);
    lv_obj_set_style_bg_color(f, lv_color_hex(0x0B0E13), 0);
    lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(f, 2, 0);
    lv_obj_set_style_border_color(f, lv_color_hex(ring), 0);
    lv_obj_set_style_pad_all(f, 0, 0);
    lv_obj_set_style_clip_corner(f, true, 0);

    lv_obj_t *im = lv_image_create(f);
    lv_image_set_src(im, cs_avatar_image(size));
    lv_image_set_scale(im, (uint32_t)(256 * size / native));
    lv_obj_center(im);
}
