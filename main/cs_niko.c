// cs_niko.c —— NiKo 专属看板(深色电竞风)+ 状态驱动的按键机。
//
// 五个屏:当前比赛(实时比分) / 队伍战绩(仅法尔孔) / NiKo 生涯(两页) /
//         设置 / 配网信息(文字页 + 二维码页)。
// 复用:font_cn16 中文字库、cs_logos 队标、cs_data(比赛+生涯)、cs_net(联网)、
//        cs_portal(配网)、cs_avatar(真实头像位图)、bsp_battery(电量)。
//
// ── 按键映射(三个键各有 短按/长按,互不冲突;全部只改状态,不做重活) ──
//  页面        上下短按        上下长按        OK单击        OK双击    OK长按
//  当前比赛    切换场次        上/下一屏       进入个人数据   立即刷新   进入设置
//  队伍战绩    切换场次        上/下一屏       返回首页       立即刷新   进入设置
//   NiKo生涯    翻页(1/2)      上/下一屏       返回首页       立即刷新   进入设置
//   设置       移动选项        上/下一屏       执行选中项     立即刷新   返回上一屏
//  配网信息     文字/二维码页   上/下一屏       返回设置       立即刷新   返回首页
//
// 历史 bug:上下键在 V_SET 落到「换页」的兜底分支,导致配网页按上下会跳到比赛页。
// 现在每个页面都必须显式处理上下键 —— 不设隐式兜底,漏了就编不过(switch 无 default)。
#include "cs_niko.h"
#include "cs_data.h"
#include "cs_net.h"
#include "cs_portal.h"
#include "cs_logos.h"
#include "cs_fonts.h"
#include "cs_avatar.h"
#include "bsp_battery.h"
#include "bsp_display.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "lvgl.h"

static const char *TAG = "cs_niko";

// ---------------------------------------------------------------------------
// 配色 / 尺寸
// ---------------------------------------------------------------------------
#define C_BG     0x0E1116
#define C_PANEL  0x1A2029
#define C_LINE   0x2A3441
#define C_INK    0xE6EDF3
#define C_MUTED  0x8FA3B8
#define C_BLUE   0x2F81F7
#define C_GREEN  0x2EA043
#define C_RED    0xE5484D
#define C_GOLD   0xE4AE39
#define C_AMBER  0xD29922

#define SCR_W    240
#define SCR_H    320
#define BAR_H    24
#define HINT_H   22
#define CONT_H   (SCR_H - BAR_H - HINT_H)

// 三键共用的 ADC 引脚 = GPIO0(见 components/bsp/include/bsp_pins.h)。
// 深度睡眠靠它唤醒:ESP32-C3 没有 EXT0/EXT1,只能用 GPIO 低电平唤醒(GPIO0~5)。
#define NK_BTN_GPIO  0

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
typedef enum {
    V_MATCH = 0,    // 当前比赛
    V_HIST,         // 队伍战绩(仅法尔孔)
    V_CAREER,       // NiKo 生涯(两页)
    V_SET,          // 设置
    V_PORTAL,       // 配网信息(不在上下长按的循环里,从设置进入)
    V_COUNT
} v_t;

// 上下长按循环的页面顺序(配网页不参与,只从设置进)
static const v_t PAGE_RING[] = { V_MATCH, V_HIST, V_CAREER, V_SET };
#define PAGE_RING_N ((int)(sizeof(PAGE_RING) / sizeof(PAGE_RING[0])))

#define SET_ITEMS    4
#define CAREER_PAGES 2
#define PORTAL_PAGES 2

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static int      s_view = V_MATCH;
static int      s_back = V_MATCH;   // 设置页长按返回的目标
static int      s_sel;              // 比赛列表:第几场
static int      s_set_sel;          // 设置:第几项
static int      s_career_page;      // 生涯:第几页
static int      s_portal_page;      // 配网:文字页/二维码页
static bool     s_dirty = true;
static uint32_t s_seen_rev = 0;
static int64_t  s_last_active;
static bool     s_ignore_click;

static int  s_idx_live[CS_MAX_MATCHES];
static int  s_n_live;
static int  s_idx_hist[CS_MAX_MATCHES];
static int  s_n_hist;
static bool s_scope_all;        // true = 法尔孔无场次,当前显示的是全部比赛
static bool s_home_upcoming;    // true = 首页没有 live,显示的是下一场

static lv_obj_t *s_scr;
static lv_obj_t *s_bar_l, *s_bar_c, *s_bar_r, *s_bar_batt, *s_bar_line;
static lv_obj_t *s_content, *s_hint;

static int  s_batt = -2;
static int  s_batt_age;
static bool s_batt_ok;
static bool s_portal_on;
static uint32_t s_last_refresh;

// 配网页二维码(生成的静态位图,内容 = 门户地址)
extern const int      cs_qr_w;
extern const int      cs_qr_h;
extern const uint16_t cs_qr565[];
static lv_image_dsc_t s_qr_dsc;
static bool           s_qr_ready;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static void scpy(char *d, size_t c, const char *s)
{
    if (!d || c == 0) return;
    if (!s) { d[0] = 0; return; }
    size_t n = strlen(s);
    if (n > c - 1) n = c - 1;
    memcpy(d, s, n);
    d[n] = 0;
}

static bool ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static lv_obj_t *panel(lv_obj_t *p, int x, int y, int w, int h,
                       uint32_t bg, uint32_t br, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, br ? 1 : 0, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(br ? br : bg), 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *p, int x, int y, const char *t,
                       const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t ? t : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void one_line(lv_obj_t *l, int w)
{
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

// 队标:命中真彩队标就贴图,否则用「队色盾牌 + 缩写」兜底
static void nk_logo(lv_obj_t *p, int x, int y, const char *id, const char *name,
                    bool small, uint32_t color)
{
    int edge = small ? 20 : 48;
    const lv_image_dsc_t *d = small ? cs_logo_get_small_named(id, name)
                                    : cs_logo_get_named(id, name);
    if (d) {
        lv_obj_t *im = lv_image_create(p);
        lv_image_set_src(im, d);
        lv_obj_set_pos(im, x, y);
        return;
    }
    lv_obj_t *bx = lv_obj_create(p);
    lv_obj_remove_flag(bx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bx, x, y);
    lv_obj_set_size(bx, edge, edge);
    lv_obj_set_style_radius(bx, edge / 4, 0);
    lv_obj_set_style_bg_color(bx, lv_color_hex(color ? color : 0x3A4553), 0);
    lv_obj_set_style_bg_opa(bx, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bx, 0, 0);
    lv_obj_set_style_pad_all(bx, 0, 0);
    char t[6];
    snprintf(t, sizeof(t), "%.3s", (name && name[0]) ? name : "?");
    lv_obj_t *lb = lv_label_create(bx);
    lv_obj_set_style_text_font(lb, small ? &lv_font_montserrat_14 : &font_cn16, 0);
    lv_obj_set_style_text_color(lb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_width(lb, edge);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
}

static uint32_t map_color(const char *en)
{
    static const struct { const char *n; uint32_t c; } M[] = {
        { "Mirage", 0xC7A76A }, { "Inferno", 0xD9632F }, { "Nuke", 0x7FB069 },
        { "Dust2", 0xD8B36A }, { "Ancient", 0x3E9E8F }, { "Anubis", 0x2F81F7 },
        { "Overpass", 0x6B8E23 }, { "Vertigo", 0x8C8C99 }, { "Train", 0x9AA5B1 },
        { "Dust", 0xD8B36A },
    };
    for (size_t i = 0; i < sizeof(M) / sizeof(M[0]); i++)
        if (ieq(M[i].n, en)) return M[i].c;
    return 0x3A4553;
}

static uint32_t status_color(const char *st)
{
    if (ieq(st, CS_ST_LIVE))     return C_RED;
    if (ieq(st, CS_ST_FINISHED)) return C_MUTED;
    return C_BLUE;
}

static const char *status_cn(const char *st)
{
    if (ieq(st, CS_ST_LIVE))     return "进行中";
    if (ieq(st, CS_ST_FINISHED)) return "已结束";
    return "未开始";
}

// ---------------------------------------------------------------------------
// 索引
//  - 首页:先法尔孔 live -> 没有则法尔孔 upcoming -> 再没有才回退全局 live
//    (数据源是 bo3.gg 全局赛程,法尔孔经常没有场次;不回退首页就常年空白)
//  - 战绩:只收法尔孔已结束的场次,绝不回退(要求:战绩内容必须全部与法尔孔相关)
// ---------------------------------------------------------------------------
static bool is_follow(const cs_match_t *m, const char *ft)
{
    if (!m || !ft || !ft[0]) return false;
    return ieq(m->t1_name, ft) || ieq(m->t2_name, ft) ||
           ieq(m->t1_short, ft) || ieq(m->t2_short, ft);
}

static int collect(const char *st, const char *ft, int *out, int max, bool allow_all)
{
    const cs_data_t *d = cs_data();
    int n = 0;
    if (ft && ft[0]) {
        for (int i = 0; i < d->count && n < max; i++)
            if (ieq(d->m[i].status, st) && is_follow(&d->m[i], ft)) out[n++] = i;
        if (n) return n;
    }
    if (!allow_all) return 0;
    for (int i = 0; i < d->count && n < max; i++)
        if (!st || ieq(d->m[i].status, st)) out[n++] = i;
    return n;
}

static void refresh_indices(void)
{
    const char *ft = cs_follow_team();
    const cs_data_t *d = cs_data();

    // 战绩:只看法尔孔,没有就是空,不回退
    s_n_hist = collect(CS_ST_FINISHED, ft, s_idx_hist, CS_MAX_MATCHES, false);

    // 首页:live -> upcoming -> 全局 live
    s_home_upcoming = false;
    s_n_live = collect(CS_ST_LIVE, ft, s_idx_live, CS_MAX_MATCHES, false);
    if (s_n_live == 0) {
        s_n_live = collect(CS_ST_UPCOMING, ft, s_idx_live, CS_MAX_MATCHES, false);
        s_home_upcoming = (s_n_live > 0);
    }

    int mine = 0;
    for (int i = 0; i < d->count; i++)
        if (is_follow(&d->m[i], ft)) { mine = 1; break; }

    if (s_n_live == 0) {
        s_n_live = collect(CS_ST_LIVE, NULL, s_idx_live, CS_MAX_MATCHES, true);
        s_home_upcoming = false;
    }
    s_scope_all = (s_n_live > 0 && !s_home_upcoming && mine == 0);

    int cap = (s_view == V_HIST) ? s_n_hist : s_n_live;
    if (s_sel >= cap) s_sel = 0;
    if (s_sel < 0)    s_sel = 0;
}

// ---------------------------------------------------------------------------
// 状态栏
// ---------------------------------------------------------------------------
static void render_status(void)
{
    char buf[80];

    int cnt = (s_view == V_HIST) ? s_n_hist : s_n_live;
    if (s_view == V_HIST) {
        if (cnt > 1) snprintf(buf, sizeof(buf), "战绩%d/%d", s_sel + 1, cnt);
        else         scpy(buf, sizeof(buf), "队伍战绩");
    } else if (s_view == V_CAREER) {
        snprintf(buf, sizeof(buf), "生涯%d/%d", s_career_page + 1, CAREER_PAGES);
    } else if (s_view == V_PORTAL) {
        snprintf(buf, sizeof(buf), "配网%d/%d", s_portal_page + 1, PORTAL_PAGES);
    } else if (s_view == V_SET) {
        scpy(buf, sizeof(buf), "设置");
    } else if (s_scope_all) {
        if (cnt > 1) snprintf(buf, sizeof(buf), "全部%d/%d", s_sel + 1, cnt);
        else         scpy(buf, sizeof(buf), "全部比赛");
    } else if (s_home_upcoming) {
        if (cnt > 1) snprintf(buf, sizeof(buf), "下轮%d/%d", s_sel + 1, cnt);
        else         scpy(buf, sizeof(buf), "下一场");
    } else if (cnt > 1) {
        snprintf(buf, sizeof(buf), "当前%d/%d", s_sel + 1, cnt);
    } else {
        scpy(buf, sizeof(buf), "当前比赛");
    }
    lv_label_set_text(s_bar_l, buf);

    const char *src = cs_data_source();
    const char *srcn = ieq(src, "cache") ? "缓存"
                     : ieq(src, "手机") ? "手机"
                     : ieq(src, "builtin") ? "示例"
                     : (src && src[0]) ? "网络" : "示例";
    if (s_portal_on)               snprintf(buf, sizeof(buf), "门户 %s", cs_portal_ssid());
    else if (cs_net_online())      snprintf(buf, sizeof(buf), "在线 %s", srcn);
    else if (cs_net_state() == CS_NET_CONNECTING) scpy(buf, sizeof(buf), "连接中");
    else if (cs_net_scan_busy())   scpy(buf, sizeof(buf), "扫描中");
    else                           snprintf(buf, sizeof(buf), "离线 %s", srcn);
    lv_label_set_text(s_bar_c, buf);

    char tm[8];
    cs_time_hhmm(tm, sizeof(tm));
    lv_label_set_text(s_bar_r, tm);

    if (++s_batt_age >= 10 || s_batt == -2) {
        s_batt_age = 0;
        s_batt = s_batt_ok ? bsp_battery_soc() : -1;
    }
    if (s_batt >= 0) {
        snprintf(buf, sizeof(buf), "%d%%", s_batt > 100 ? 100 : s_batt);
        lv_label_set_text(s_bar_batt, buf);
        uint32_t bc = s_batt <= 15 ? C_RED : (s_batt <= 40 ? C_AMBER : C_GREEN);
        lv_obj_set_style_text_color(s_bar_batt, lv_color_hex(bc), 0);
    } else {
        lv_label_set_text(s_bar_batt, "");
    }

    uint32_t lc = C_MUTED;
    cs_diag_t diag = cs_net_diag();
    if (diag != CS_DIAG_NONE)      lc = C_AMBER;
    else if (cs_net_online())      lc = C_GREEN;
    else if (cs_net_state() == CS_NET_CONNECTING) lc = C_BLUE;
    lv_obj_set_style_bg_color(s_bar_line, lv_color_hex(lc), 0);
}

// ---------------------------------------------------------------------------
// 复用块:比赛对战牌(双方队标 + 比分)
// ---------------------------------------------------------------------------
static void draw_versus(lv_obj_t *p, const cs_match_t *m, int y)
{
    nk_logo(p, 6,   y, m->t1_logo, m->t1_name, false, m->t1_color);
    nk_logo(p, 186, y, m->t2_logo, m->t2_name, false, m->t2_color);

    lv_obj_t *n1 = label(p, 58, y + 2, m->t1_name, &font_cn16, C_INK);
    one_line(n1, 62);
    lv_obj_set_style_text_align(n1, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *n2 = label(p, 120, y + 2, m->t2_name, &font_cn16, C_INK);
    one_line(n2, 62);

    char buf[32];
    bool played = (m->score1 != 0 || m->score2 != 0 || ieq(m->status, CS_ST_FINISHED));
    if (played) snprintf(buf, sizeof(buf), "%d-%d", m->score1 % 1000, m->score2 % 1000);
    else        scpy(buf, sizeof(buf), "VS");
    lv_obj_t *sc = label(p, 58, y + 20, buf, &lv_font_montserrat_14, C_INK);
    lv_obj_set_width(sc, 124);
    lv_obj_set_style_text_align(sc, LV_TEXT_ALIGN_CENTER, 0);
}

// 复用块:地图比分小条;返回下一行的 y
static int draw_maps(lv_obj_t *p, const cs_match_t *m, int y, int row_h, bool dots)
{
    char buf[32];
    for (int i = 0; i < m->map_count && i < CS_MAX_MAPS; i++) {
        const cs_map_t *mp = &m->maps[i];
        if (y + row_h > CONT_H) break;
        panel(p, 6, y, SCR_W - 12, row_h, C_PANEL, C_LINE, 6);
        panel(p, 10, y + (row_h - 12) / 2, 3, 12, map_color(mp->name), map_color(mp->name), 0);
        lv_obj_t *nm = label(p, 18, y + (row_h - 16) / 2, mp->cn[0] ? mp->cn : mp->name,
                             &font_cn16, C_INK);
        one_line(nm, 118);
        bool played = (mp->s1 || mp->s2);
        if (played) snprintf(buf, sizeof(buf), "%d - %d", mp->s1 % 1000, mp->s2 % 1000);
        else        scpy(buf, sizeof(buf), "-");
        uint32_t mc = !played ? C_MUTED
                    : (mp->winner == 1 ? C_GREEN : (mp->winner == 2 ? C_GOLD : C_MUTED));
        lv_obj_t *ms = label(p, 142, y + (row_h - 16) / 2, buf, &font_cn16, mc);
        lv_obj_set_width(ms, 90);
        lv_obj_set_style_text_align(ms, LV_TEXT_ALIGN_RIGHT, 0);
        if (dots) (void)0;
        y += row_h + 1;
    }
    return y;
}

// ---------------------------------------------------------------------------
// 屏:当前比赛
// ---------------------------------------------------------------------------
static void build_match(lv_obj_t *p)
{
    refresh_indices();
    const cs_niko_t *n = cs_niko();
    char buf[96];

    // ── 顶部:真实头像 + 选手概要(把原先空着的一大块用掉) ──
    cs_avatar_draw(p, 4, 2, 84);

    lv_obj_t *nm = label(p, 96, 6, (n && n->name[0]) ? n->name : "NiKo", &font_cn16, C_GOLD);
    one_line(nm, 140);

    const char *ft = cs_follow_team();
    if (ft && ft[0]) {
        nk_logo(p, 96, 28, (n && n->team_logo[0]) ? n->team_logo : NULL, ft, true,
                (n ? n->team_color : 0));
        lv_obj_t *tm = label(p, 120, 30, ft, &font_cn16, C_INK);
        one_line(tm, 116);
    }
    if (n && n->role[0]) {
        lv_obj_t *rl = label(p, 96, 52, n->role, &font_cn16, C_MUTED);
        one_line(rl, 140);
    }
    if (n) {
        snprintf(buf, sizeof(buf), "RATING %.2f  MVP %d", n->rating, n->mvp);
        lv_obj_t *rt = label(p, 96, 72, buf, &font_cn16, C_MUTED);
        one_line(rt, 140);
    }

    // ── 比赛牌 ──
    int top = 92;
    if (s_n_live == 0) {
        panel(p, 4, top, SCR_W - 8, 96, C_PANEL, C_LINE, 8);
        lv_obj_t *t = label(p, 12, top + 20, "暂无法尔孔赛程", &font_cn16, C_MUTED);
        one_line(t, SCR_W - 24);
        if (ft && ft[0]) {
            lv_obj_t *f = label(p, 12, top + 44, ft, &font_cn16, C_GOLD);
            one_line(f, SCR_W - 24);
        }
        lv_obj_t *h = label(p, 12, top + 68, "长按上下键换屏", &font_cn16, C_MUTED);
        one_line(h, SCR_W - 24);
        return;
    }

    const cs_match_t *m = &cs_data()->m[s_idx_live[s_sel % s_n_live]];

    lv_obj_t *ev = label(p, 4, top, m->event, &font_cn16, C_INK);
    one_line(ev, SCR_W - 8);
    lv_obj_set_style_text_align(ev, LV_TEXT_ALIGN_CENTER, 0);

    snprintf(buf, sizeof(buf), "%s %s · %s", status_cn(m->status), m->bo, m->date);
    lv_obj_t *sub = label(p, 4, top + 18, buf, &font_cn16, status_color(m->status));
    one_line(sub, SCR_W - 8);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    draw_versus(p, m, top + 38);
    draw_maps(p, m, top + 92, 22, false);
}

// ---------------------------------------------------------------------------
// 屏:队伍战绩(仅法尔孔)
// ---------------------------------------------------------------------------
static void build_hist(lv_obj_t *p)
{
    refresh_indices();
    const char *ft = cs_follow_team();
    char buf[96];

    if (s_n_hist == 0) {
        lv_obj_t *t = label(p, 4, 96, "暂无法尔孔战绩", &font_cn16, C_MUTED);
        one_line(t, SCR_W - 8);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *h = label(p, 4, 124, "已结束的场次才会进这里", &font_cn16, C_MUTED);
        one_line(h, SCR_W - 8);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
        if (ft && ft[0]) {
            lv_obj_t *f = label(p, 4, 152, ft, &font_cn16, C_GOLD);
            one_line(f, SCR_W - 8);
            lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
        }
        return;
    }

    const cs_match_t *m = &cs_data()->m[s_idx_hist[s_sel % s_n_hist]];

    bool my_t1 = ft && ieq(m->t1_name, ft);
    bool my_t2 = ft && ieq(m->t2_name, ft);
    bool win  = (my_t1 && m->score1 > m->score2) || (my_t2 && m->score2 > m->score1);
    bool loss = (my_t1 && m->score1 < m->score2) || (my_t2 && m->score2 < m->score1);

    // 首行:胜负标签 + 赛事名
    snprintf(buf, sizeof(buf), "%s", win ? "胜" : (loss ? "负" : "平"));
    lv_obj_t *tg = label(p, 6, 2, buf, &font_cn16, win ? C_GREEN : C_RED);
    one_line(tg, 34);
    lv_obj_t *ev = label(p, 42, 2, m->event, &font_cn16, C_INK);
    one_line(ev, SCR_W - 48);

    draw_versus(p, m, 24);

    // 日期 + 赛制
    snprintf(buf, sizeof(buf), "%s · %s · 第 %d/%d 场", m->date, m->bo,
             s_sel + 1, s_n_hist);
    lv_obj_t *sb = label(p, 4, 74, buf, &font_cn16, C_MUTED);
    one_line(sb, SCR_W - 8);
    lv_obj_set_style_text_align(sb, LV_TEXT_ALIGN_CENTER, 0);

    draw_maps(p, m, 96, 24, false);
}

// ---------------------------------------------------------------------------
// 屏:NiKo 生涯(两页)
// ---------------------------------------------------------------------------
static void stat_card(lv_obj_t *p, int x, int y, int w,
                      const char *k, const char *v, uint32_t vc)
{
    panel(p, x, y, w, 50, C_PANEL, C_LINE, 8);
    lv_obj_t *lk = label(p, x + 10, y + 6, k, &font_cn16, C_MUTED);
    one_line(lk, w - 20);
    lv_obj_t *lv2 = label(p, x + 10, y + 26, v, &font_cn16, vc);
    one_line(lv2, w - 20);
}

static void build_career(lv_obj_t *p)
{
    const cs_niko_t *n = cs_niko();
    const char *ft = cs_follow_team();
    char v[24];
    char buf[64];

    cs_avatar_draw(p, 4, 2, 104);

    lv_obj_t *nm = label(p, 116, 8, (n && n->name[0]) ? n->name : "NiKo", &font_cn16, C_GOLD);
    one_line(nm, 120);
    if (n && n->realname[0]) {
        lv_obj_t *rn = label(p, 116, 30, n->realname, &font_cn16, C_INK);
        one_line(rn, 120);
    }
    if (ft && ft[0]) {
        nk_logo(p, 116, 52, (n && n->team_logo[0]) ? n->team_logo : NULL, ft, true,
                (n ? n->team_color : 0));
        lv_obj_t *tm = label(p, 140, 54, ft, &font_cn16, C_INK);
        one_line(tm, 96);
    }
    if (n && n->role[0]) {
        lv_obj_t *rl = label(p, 116, 78, n->role, &font_cn16, C_MUTED);
        one_line(rl, 120);
    }

    const int X1 = 4, X2 = 124, W = 112;
    const int Y1 = 116, Y2 = 170, Y3 = 224;

    if (s_career_page == 0) {
        if (n) {
            snprintf(v, sizeof(v), "%.2f", n->rating);
            stat_card(p, X1, Y1, W, "Rating", v, C_GOLD);
            snprintf(v, sizeof(v), "%.2f", n->kd);
            stat_card(p, X2, Y1, W, "K/D", v, C_INK);
            snprintf(v, sizeof(v), "%.1f", n->adr);
            stat_card(p, X1, Y2, W, "ADR", v, C_INK);
            snprintf(v, sizeof(v), "%.1f%%", n->kast);
            stat_card(p, X2, Y2, W, "KAST", v, C_INK);
            snprintf(v, sizeof(v), "%.2f", n->impact);
            stat_card(p, X1, Y3, W, "Impact", v, C_INK);
            snprintf(v, sizeof(v), "%d", n->maps);
            stat_card(p, X2, Y3, W, "出场地图", v, C_INK);
        } else {
            lv_obj_t *e = label(p, 4, Y1, "无生涯数据", &font_cn16, C_MUTED);
            one_line(e, SCR_W - 8);
        }
    } else {
        if (n) {
            snprintf(v, sizeof(v), "%d", n->mvp);
            stat_card(p, X1, Y1, W, "MVP 次数", v, C_GOLD);
            snprintf(v, sizeof(v), "%d", n->majors);
            stat_card(p, X2, Y1, W, "大赛冠军", v, C_INK);
            snprintf(v, sizeof(v), "%d 万$", n->earnings);
            stat_card(p, X1, Y2, W, "总奖金", v, C_INK);
            snprintf(v, sizeof(v), "%d 岁", n->age);
            stat_card(p, X2, Y2, W, "年龄", v, C_INK);
            snprintf(buf, sizeof(buf), "%s", n->years[0] ? n->years : "—");
            stat_card(p, X1, Y3, W * 2 + 8, "职业生涯", buf, C_MUTED);
        } else {
            lv_obj_t *e = label(p, 4, Y1, "无生涯数据", &font_cn16, C_MUTED);
            one_line(e, SCR_W - 8);
        }
    }
}

// ---------------------------------------------------------------------------
// 屏:设置(上下键必须在本页内移动选项 —— 历史上这里会跳到比赛页)
// ---------------------------------------------------------------------------
static void build_set(lv_obj_t *p)
{
    char buf[96];
    panel(p, 4, 2, SCR_W - 8, 88, C_PANEL, C_LINE, 8);

    const char *ssid = cs_net_ssid();
    char wbuf[80];
    if (cs_net_online())           snprintf(wbuf, sizeof(wbuf), "%s %s", ssid[0] ? ssid : "已连接", cs_net_ip());
    else if (cs_net_state() == CS_NET_CONNECTING) snprintf(wbuf, sizeof(wbuf), "%s 连接中", ssid[0] ? ssid : "Wi-Fi");
    else if (cs_net_state() == CS_NET_FAILED)     snprintf(wbuf, sizeof(wbuf), "失败:%s", cs_net_conn_err());
    else                                          scpy(wbuf, sizeof(wbuf), "未连接");
    lv_obj_t *lk = label(p, 12, 8, "网络", &font_cn16, C_MUTED);
    one_line(lk, 40);
    lv_obj_t *lv2 = label(p, 56, 8, wbuf, &font_cn16, cs_net_online() ? C_GREEN : C_INK);
    one_line(lv2, SCR_W - 68);

    const cs_data_t *d = cs_data();
    snprintf(buf, sizeof(buf), "%d 场 · %s", d->count % 10000, d->updated);
    lv_obj_t *lk2 = label(p, 12, 30, "数据", &font_cn16, C_MUTED);
    one_line(lk2, 40);
    lv_obj_t *lv3 = label(p, 56, 30, buf, &font_cn16, C_INK);
    one_line(lv3, SCR_W - 68);

    cs_diag_t diag = cs_net_diag();
    lv_obj_t *lk3 = label(p, 12, 52, "诊断", &font_cn16, C_MUTED);
    one_line(lk3, 40);
    lv_obj_t *lv4 = label(p, 56, 52, cs_net_diag_text(), &font_cn16,
                          diag == CS_DIAG_NONE ? C_GREEN : C_AMBER);
    one_line(lv4, SCR_W - 68);

    const char *mir = cs_net_mirror_name();
    snprintf(buf, sizeof(buf), "%s%s", mir[0] ? "镜像 " : "", mir[0] ? mir : "内置/手机");
    lv_obj_t *lk5 = label(p, 12, 72, "来源", &font_cn16, C_MUTED);
    one_line(lk5, 40);
    lv_obj_t *lv5 = label(p, 56, 72, buf, &font_cn16, C_MUTED);
    one_line(lv5, SCR_W - 68);

    const char *items[SET_ITEMS];
    char item0[40];
    scpy(item0, sizeof(item0), s_portal_on ? "关闭配网门户" : "打开配网门户");
    items[0] = item0;
    items[1] = "立即刷新数据";
    items[2] = "清除缓存回示例";
    items[3] = "配网信息/二维码";

    int y = 96;
    for (int i = 0; i < SET_ITEMS; i++) {
        int ry = y + i * 44;
        if (ry + 40 > CONT_H) break;
        bool sel = (i == s_set_sel);
        panel(p, 4, ry, SCR_W - 8, 40, sel ? 0x1B2E4A : C_PANEL, sel ? C_BLUE : C_LINE, 8);
        lv_obj_t *lb = label(p, 14, ry + 10, items[i], &font_cn16, sel ? C_INK : C_MUTED);
        one_line(lb, SCR_W - 40);
    }
}

// ---------------------------------------------------------------------------
// 屏:配网信息(文字页 / 二维码页)—— 热点名与密码必须完整可见
// ---------------------------------------------------------------------------
static void build_portal(lv_obj_t *p)
{
    char buf[96];

    if (s_portal_page == 0) {
        panel(p, 4, 2, SCR_W - 8, 22, 0x152A1C, C_GREEN, 6);
        lv_obj_t *hd = label(p, 10, 5, "配网门户已开启", &font_cn16, C_GREEN);
        one_line(hd, SCR_W - 20);

        // 热点名:独立成行 + 自动换行,杜绝"显示不全"
        lv_obj_t *k1 = label(p, 8, 32, "热点名称", &font_cn16, C_MUTED);
        one_line(k1, 60);
        lv_obj_t *v1 = label(p, 8, 54, cs_portal_ssid(), &font_cn16, C_INK);
        lv_obj_set_width(v1, SCR_W - 16);
        lv_label_set_long_mode(v1, LV_LABEL_LONG_WRAP);

        lv_obj_t *k2 = label(p, 8, 92, "热点密码", &font_cn16, C_MUTED);
        one_line(k2, 60);
        lv_obj_t *v2 = label(p, 8, 114, cs_portal_pass(), &font_cn16, C_GOLD);
        lv_obj_set_width(v2, SCR_W - 16);
        lv_label_set_long_mode(v2, LV_LABEL_LONG_WRAP);

        lv_obj_t *k3 = label(p, 8, 152, "设置台网址", &font_cn16, C_MUTED);
        one_line(k3, 80);
        lv_obj_t *v3 = label(p, 8, 174, cs_portal_url(), &font_cn16, C_INK);
        lv_obj_set_width(v3, SCR_W - 16);
        lv_label_set_long_mode(v3, LV_LABEL_LONG_WRAP);

        panel(p, 4, 206, SCR_W - 8, 62, C_PANEL, C_LINE, 8);
        lv_obj_t *t1 = label(p, 12, 214, "手机连上热点后打开网址,", &font_cn16, C_MUTED);
        one_line(t1, SCR_W - 24);
        lv_obj_t *t2 = label(p, 12, 234, "或按上下键扫码直达。", &font_cn16, C_MUTED);
        one_line(t2, SCR_W - 24);
        lv_obj_t *t3 = label(p, 12, 252, "停止配网:OK 键回设置页", &font_cn16, C_MUTED);
        one_line(t3, SCR_W - 24);
        return;
    }

    // ── 二维码页:静态二维码(内容是固定的设置台地址,已在生成期解码校验) ──
    if (!s_qr_ready) {
        memset(&s_qr_dsc, 0, sizeof(s_qr_dsc));
        s_qr_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
        s_qr_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
        s_qr_dsc.header.w      = (uint32_t)cs_qr_w;
        s_qr_dsc.header.h      = (uint32_t)cs_qr_h;
        s_qr_dsc.header.stride = (uint32_t)cs_qr_w * 2u;
        s_qr_dsc.data_size     = (uint32_t)cs_qr_w * (uint32_t)cs_qr_h * 2u;
        s_qr_dsc.data          = (const uint8_t *)cs_qr565;
        s_qr_ready = true;
    }
    panel(p, 4, 2, SCR_W - 8, 20, 0x152A1C, C_GREEN, 6);
    lv_obj_t *hd = label(p, 10, 4, "扫码打开设置台", &font_cn16, C_GREEN);
    one_line(hd, SCR_W - 20);

    lv_obj_t *qr = lv_image_create(p);
    lv_image_set_src(qr, &s_qr_dsc);
    lv_obj_set_pos(qr, (SCR_W - cs_qr_w) / 2, 28);

    snprintf(buf, sizeof(buf), "%s", cs_portal_url());
    lv_obj_t *u = label(p, 4, 28 + cs_qr_h + 6, buf, &font_cn16, C_INK);
    one_line(u, SCR_W - 8);
    lv_obj_set_style_text_align(u, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *t = label(p, 4, 28 + cs_qr_h + 28, "需先连上本机热点", &font_cn16, C_MUTED);
    one_line(t, SCR_W - 8);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------
static void render_content(void)
{
    lv_obj_clean(s_content);
    switch (s_view) {
    case V_MATCH:  build_match(s_content);  break;
    case V_HIST:   build_hist(s_content);   break;
    case V_CAREER: build_career(s_content); break;
    case V_SET:    build_set(s_content);    break;
    case V_PORTAL: build_portal(s_content); break;
    }
}

static void render_hint(void)
{
    const char *h;
    switch (s_view) {
    case V_MATCH:  h = "上下换场 OK数据 长按设置"; break;
    case V_HIST:   h = "上下换场 OK返回 长按设置"; break;
    case V_CAREER: h = "上下翻页 OK返回 长按设置"; break;
    case V_SET:    h = "上下选择 OK执行 长按返回"; break;
    case V_PORTAL: h = "上下切换 OK返回 长按首页"; break;
    default:       h = ""; break;
    }
    lv_label_set_text(s_hint, h);
}

// ---------------------------------------------------------------------------
// 按键:只改状态,界面由 on_tick 统一重绘
// ---------------------------------------------------------------------------
static void exec_set(void)
{
    switch (s_set_sel) {
    case 0:
        if (s_portal_on) {
            cs_portal_stop();
            s_portal_on = false;
            if (s_view == V_PORTAL) s_view = V_SET;
        } else if (cs_portal_start() == ESP_OK) {
            s_portal_on = true;
            s_portal_page = 0;
            s_back = s_view;            // 记下从哪来,长按返回时回去
            s_view = V_PORTAL;          // 开门即给出热点信息,少按一次
        }
        break;
    case 1: cs_net_refresh(true); break;
    case 2: cs_data_clear_cache(); cs_niko_use_builtin(); break;
    case 3:
        if (s_portal_on) { s_portal_page = 0; s_back = V_SET; s_view = V_PORTAL; }
        else             { s_set_sel = 0; }   // 门户没开:该项无效,留在原地
        break;
    default: break;
    }
    s_dirty = true;
}

// 上下短按:在本页内移动选择
static void move_sel(int dir)
{
    switch (s_view) {
    case V_MATCH:
        if (s_n_live > 1) { s_sel = (s_sel + dir + s_n_live) % s_n_live; s_dirty = true; }
        break;
    case V_HIST:
        if (s_n_hist > 1) { s_sel = (s_sel + dir + s_n_hist) % s_n_hist; s_dirty = true; }
        break;
    case V_CAREER:
        s_career_page = (s_career_page + dir + CAREER_PAGES) % CAREER_PAGES;
        s_dirty = true;
        break;
    case V_SET:
        s_set_sel = (s_set_sel + dir + SET_ITEMS) % SET_ITEMS;
        s_dirty = true;
        break;
    case V_PORTAL:
        s_portal_page = (s_portal_page + dir + PORTAL_PAGES) % PORTAL_PAGES;
        s_dirty = true;
        break;
    }
}

// 上下长按:切换屏幕(只在四个主页面之间循环,配网页不参与)
static void ring_page(int dir)
{
    int idx = 0;
    for (int i = 0; i < PAGE_RING_N; i++) if (PAGE_RING[i] == s_view) idx = i;
    idx = (idx + dir + PAGE_RING_N) % PAGE_RING_N;
    s_view = PAGE_RING[idx];
    s_sel = 0;
    s_dirty = true;
}

static void ok_click(void)
{
    switch (s_view) {
    case V_MATCH:
        s_back = V_MATCH;
        s_view = V_CAREER;            // 明确入口:首页 OK = 看 NiKo 个人数据
        break;
    case V_HIST:
    case V_CAREER:
        s_view = V_MATCH;
        break;
    case V_SET:
        exec_set();
        return;
    case V_PORTAL:
        s_view = s_back;
        break;
    }
    s_dirty = true;
}

static void ok_long(void)
{
    if (s_view == V_MATCH) {
        s_back = V_MATCH;
        s_set_sel = 0;
        s_view = V_SET;               // 首页长按 = 进设置
    } else if (s_view == V_SET) {
        s_view = (v_t)s_back;         // 设置页长按 = 返回上一屏
    } else {
        s_view = V_MATCH;             // 其它页长按 = 回首页
    }
    s_dirty = true;
}

void cs_niko_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    s_last_active = esp_timer_get_time();

    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            if (s_ignore_click) { s_ignore_click = false; return; }
            ok_click();
        } else if (ev == BSP_BTN_DOUBLE) {
            s_ignore_click = true;
            cs_net_refresh(true);
        } else if (ev == BSP_BTN_LONG) {
            s_ignore_click = true;
            ok_long();
        }
        return;                        // OK 的 PRESS 不处理,避免与单击重复
    }

    // 上下键:PRESS 立刻响应(手感好),长按换屏;CLICK/DOUBLE 一律忽略
    // —— 否则一次按下会先走 PRESS 再走 CLICK,选择跳两格。
    int dir = (btn == BSP_BTN_DOWN) ? 1 : -1;
    if (ev == BSP_BTN_PRESS)      move_sel(dir);
    else if (ev == BSP_BTN_LONG)  ring_page(dir);
}

// ---------------------------------------------------------------------------
// 200ms 定时器:网络维护 + 实时刷新 + 省电
// ---------------------------------------------------------------------------
static void on_tick(lv_timer_t *t)
{
    (void)t;
    cs_net_tick();
    refresh_indices();

    uint32_t rev = cs_data_rev();
    if (rev != s_seen_rev) { s_seen_rev = rev; s_dirty = true; }

    // 实时比分:有 live 场次且在线时每 15s 强制刷新
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    bool has_live = false;
    for (int i = 0; i < s_n_live; i++)
        if (ieq(cs_data()->m[s_idx_live[i]].status, CS_ST_LIVE)) { has_live = true; break; }
    if (cs_net_online() && has_live && (now_s - s_last_refresh) > 15) {
        s_last_refresh = now_s;
        cs_net_refresh(true);
    }

    int64_t idle = (esp_timer_get_time() - s_last_active) / 1000;
    if (s_portal_on || has_live) {
        bsp_display_backlight(100);
    } else if (idle > 600000) {
        ESP_LOGI(TAG, "空闲 10 分钟,进入深度睡眠(定时 + 按键唤醒)");
        esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL);
        gpio_config_t gc = {
            .pin_bit_mask = (1ULL << NK_BTN_GPIO),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gc);
        // ESP32-C3 无 EXT0/EXT1,走 SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP(C3 仅 GPIO0~5)
        esp_deep_sleep_enable_gpio_wakeup((uint64_t)1 << NK_BTN_GPIO,
                                          ESP_GPIO_WAKEUP_GPIO_LOW);
        esp_deep_sleep_start();
    } else if (idle > 60000) {
        bsp_display_backlight(8);
    } else {
        bsp_display_backlight(100);
    }

    render_status();
    if (s_dirty) { s_dirty = false; render_content(); render_hint(); }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
void cs_niko_start(void)
{
    cs_logos_init();
    s_batt_ok = (bsp_battery_init() == ESP_OK);
    if (!s_batt_ok) ESP_LOGW(TAG, "CW2017 不在位,状态栏不显示电量");

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    s_bar_l = label(s_scr, 6, 4, "", &font_cn16, C_INK);
    lv_obj_set_width(s_bar_l, 64); one_line(s_bar_l, 64);
    s_bar_c = label(s_scr, 72, 4, "", &font_cn16, C_MUTED);
    lv_obj_set_width(s_bar_c, 88); one_line(s_bar_c, 88);
    s_bar_r = label(s_scr, 162, 5, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s_bar_r, 40); lv_obj_set_style_text_align(s_bar_r, LV_TEXT_ALIGN_RIGHT, 0);
    s_bar_batt = label(s_scr, 204, 5, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s_bar_batt, 32); lv_obj_set_style_text_align(s_bar_batt, LV_TEXT_ALIGN_RIGHT, 0);
    s_bar_line = panel(s_scr, 0, BAR_H - 2, SCR_W, 2, C_MUTED, C_MUTED, 0);

    s_content = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_content, 0, BAR_H);
    lv_obj_set_size(s_content, SCR_W, CONT_H);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);

    s_hint = label(s_scr, 0, SCR_H - 20, "", &font_cn16, C_MUTED);
    lv_obj_set_width(s_hint, SCR_W);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);

    s_seen_rev = cs_data_rev();
    refresh_indices();
    s_last_active = esp_timer_get_time();
    s_last_refresh = (uint32_t)(s_last_active / 1000000);

    render_status();
    render_content();
    render_hint();

    lv_timer_create(on_tick, 200, NULL);
    lv_screen_load(s_scr);

    ESP_LOGI(TAG, "NiKo 看板就绪");
}
