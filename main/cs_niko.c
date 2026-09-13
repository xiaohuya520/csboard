// cs_niko.c —— NiKo 专属看板(深色电竞风)+ 状态驱动的按键机。
//
// 四屏:当前比赛(实时比分) / 队伍战绩 / NiKo 生涯 / 设置。
// 复用:font_cn16 中文字库、cs_logos 队标(已内嵌 105 支顶级战队真彩队标,
//        其余战队用「队色盾牌 + 缩写」兜底,覆盖全部世界前 200 战队)、
//        cs_data(比赛+生涯数据)、cs_net(联网)、cs_portal(配网)、bsp_battery(电量)。
//
// 省电 + 实时:有进行中比赛时每 15s 强制刷新(实时比分);空闲 60s 暗屏、
// 10min 深度睡眠(定时+按键唤醒),非 live 时靠 cs_net 5min 轮询。
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
// 配色 / 尺寸(沿用 cs_app 已验证的暗色电竞主题)
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

// 三键共用的 ADC 引脚 = GPIO0(见 components/bsp/include/bsp_pins.h
// 的 BSP_BTN_ADC_CHANNEL=ADC_CHANNEL_0)。深度睡眠靠它唤醒。
#define NK_BTN_GPIO  0

// ---------------------------------------------------------------------------
// 视图
// ---------------------------------------------------------------------------
typedef enum {
    V_MATCH = 0, V_HIST, V_CAREER, V_SET, V_COUNT
} v_t;

static const char *V_NAME[V_COUNT] = { "当前比赛", "队伍战绩", "NiKo生涯", "设置" };

#define SET_ITEMS 3

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static int      s_view = V_MATCH;
static int      s_sel;          // 比赛列表:第几场
static int      s_set_sel;      // 设置:第几项
static bool     s_dirty = true;
static uint32_t s_seen_rev = 0;
static int64_t  s_last_active;
static bool     s_ignore_click;

static int  s_idx_live[CS_MAX_MATCHES];
static int  s_n_live;
static int  s_idx_hist[CS_MAX_MATCHES];
static int  s_n_hist;
static bool s_scope_all;        // true = 关注战队无场次,当前显示的是全部比赛

static lv_obj_t *s_scr;
static lv_obj_t *s_bar_l, *s_bar_c, *s_bar_r, *s_bar_batt, *s_bar_line;
static lv_obj_t *s_content, *s_hint;

static int  s_batt = -2;
static int  s_batt_age;
static bool s_batt_ok;
static bool s_portal_on;
static uint32_t s_last_refresh;

// ---------------------------------------------------------------------------
// 小工具(与 cs_app 一致的写法,确保 API 兼容)
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

// 队标:找到真彩队标就用,否则用队色盾牌 + 缩写(覆盖全部世界前 200 战队)
static void nk_logo(lv_obj_t *p, int x, int y, const char *id, const char *name,
                    bool small, uint32_t color)
{
    int edge = small ? 20 : 48;
    const lv_image_dsc_t *d = small ? cs_logo_get_small(id) : cs_logo_get(id);
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
// 索引:优先挑"关注战队"的比赛;该状态下一场都没有时,才回退到全部比赛。
// 数据源是 bo3.gg 的全局赛程,未必时刻有 Falcons 的场次,所以必须留这个兜底,
// 否则主界面大多数时候是空的。
// ---------------------------------------------------------------------------
static bool is_follow(const cs_match_t *m, const char *ft)
{
    if (!m || !ft || !ft[0]) return false;
    return ieq(m->t1_name, ft) || ieq(m->t2_name, ft) ||
           ieq(m->t1_short, ft) || ieq(m->t2_short, ft);
}

static int collect(const char *st, const char *ft, int *out, int max)
{
    const cs_data_t *d = cs_data();
    int n = 0;
    if (ft && ft[0]) {
        for (int i = 0; i < d->count && n < max; i++)
            if (ieq(d->m[i].status, st) && is_follow(&d->m[i], ft)) out[n++] = i;
        if (n) return n;
    }
    for (int i = 0; i < d->count && n < max; i++)
        if (!st || ieq(d->m[i].status, st)) out[n++] = i;
    return n;
}

static void refresh_indices(void)
{
    const char *ft = cs_follow_team();
    s_n_live = collect(CS_ST_LIVE,     ft, s_idx_live, CS_MAX_MATCHES);
    s_n_hist = collect(CS_ST_FINISHED, ft, s_idx_hist, CS_MAX_MATCHES);

    // 关注战队在当前状态下没有场次 → 回退成全局视图(状态栏会标"全部")
    const cs_data_t *d = cs_data();
    int mine = 0;
    for (int i = 0; i < d->count; i++)
        if (is_follow(&d->m[i], ft)) { mine = 1; break; }
    s_scope_all = (mine == 0);

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
    if (s_scope_all) {
        // 关注战队没有场次 → 明确告诉用户当前看的是全局赛程
        if (cnt > 1) snprintf(buf, sizeof(buf), "全部%d/%d", s_sel + 1, cnt);
        else         scpy(buf, sizeof(buf), "全部比赛");
    } else if (cnt > 1) {
        snprintf(buf, sizeof(buf), "%s%d/%d",
                 (s_view == V_HIST) ? "战绩" : "当前", s_sel + 1, cnt);
    } else {
        scpy(buf, sizeof(buf), V_NAME[s_view]);
    }
    lv_label_set_text(s_bar_l, buf);

    const char *src = cs_data_source();
    const char *srcn = ieq(src, "cache") ? "缓存"
                     : ieq(src, "手机") ? "手机"
                     : ieq(src, "builtin") ? "示例"
                     : (src && src[0]) ? "网络" : "示例";
    if (cs_portal_active())        snprintf(buf, sizeof(buf), "门户 %s", cs_portal_ssid());
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
// 屏幕:当前比赛(实时比分)
// ---------------------------------------------------------------------------
static void build_match(lv_obj_t *p)
{
    refresh_indices();
    cs_avatar_draw(p, 192, 2, 42);   // 右上角头像

    if (s_n_live == 0) {
        lv_obj_t *t = label(p, 6, 108, "暂无进行中的比赛", &font_cn16, C_MUTED);
        one_line(t, 228);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        const char *ft = cs_follow_team();
        if (ft && ft[0]) {
            lv_obj_t *f = label(p, 6, 148, ft, &font_cn16, C_GOLD);
            one_line(f, 228);
            lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
        }
        return;
    }

    int mi = s_idx_live[s_sel % s_n_live];
    const cs_match_t *m = &cs_data()->m[mi];
    char buf[96];

    lv_obj_t *ev = label(p, 6, 2, m->event, &font_cn16, C_INK);
    one_line(ev, 180);
    lv_obj_set_style_text_align(ev, LV_TEXT_ALIGN_CENTER, 0);
    snprintf(buf, sizeof(buf), "%s · %s", m->stage, m->bo);
    lv_obj_t *sub = label(p, 6, 24, buf, &font_cn16, C_MUTED);
    one_line(sub, 228);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *n1 = label(p, 6, 50, m->t1_name, &font_cn16, C_INK);
    lv_obj_set_width(n1, 104); one_line(n1, 104);
    lv_obj_t *n2 = label(p, 130, 50, m->t2_name, &font_cn16, C_INK);
    lv_obj_set_width(n2, 104); lv_obj_set_style_text_align(n2, LV_TEXT_ALIGN_RIGHT, 0);
    one_line(n2, 104);

    nk_logo(p, 6, 72, m->t1_logo, m->t1_name, false, m->t1_color);
    nk_logo(p, 186, 72, m->t2_logo, m->t2_name, false, m->t2_color);

    snprintf(buf, sizeof(buf), "%d : %d", m->score1 % 1000, m->score2 % 1000);
    lv_obj_t *sc = label(p, 56, 80, buf, &lv_font_montserrat_28, C_INK);
    lv_obj_set_width(sc, 128);
    lv_obj_set_style_text_align(sc, LV_TEXT_ALIGN_CENTER, 0);

    uint32_t scol = status_color(m->status);
    lv_obj_t *st = label(p, 6, 122, status_cn(m->status), &font_cn16, scol);
    one_line(st, 228);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);

    int y = 146;
    for (int i = 0; i < m->map_count && i < CS_MAX_MAPS; i++) {
        const cs_map_t *mp = &m->maps[i];
        panel(p, 6, y, 228, 22, C_PANEL, C_LINE, 6);
        panel(p, 10, y + 5, 3, 12, map_color(mp->name), map_color(mp->name), 0);
        lv_obj_t *nm = label(p, 18, y + 3, mp->cn[0] ? mp->cn : mp->name, &font_cn16, C_INK);
        one_line(nm, 110);
        bool played = (mp->s1 || mp->s2);
        if (played) snprintf(buf, sizeof(buf), "%d - %d", mp->s1 % 1000, mp->s2 % 1000);
        else        scpy(buf, sizeof(buf), "-");
        uint32_t mc = !played ? C_MUTED
                    : (mp->winner == 1 ? C_GREEN : (mp->winner == 2 ? C_GOLD : C_MUTED));
        lv_obj_t *ms = label(p, 132, y + 3, buf, &font_cn16, mc);
        lv_obj_set_width(ms, 96);
        lv_obj_set_style_text_align(ms, LV_TEXT_ALIGN_RIGHT, 0);
        y += 24;
    }
}

// ---------------------------------------------------------------------------
// 屏幕:队伍战绩
// ---------------------------------------------------------------------------
static void build_hist(lv_obj_t *p)
{
    refresh_indices();
    const char *ft = cs_follow_team();

    if (s_n_hist == 0) {
        lv_obj_t *t = label(p, 6, 120, "暂无队伍战绩", &font_cn16, C_MUTED);
        one_line(t, 228);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int base = 4;
    if (s_n_hist > 1) {
        char h[64];
        snprintf(h, sizeof(h), "队伍战绩 %d/%d · OK翻看", s_sel + 1, s_n_hist);
        lv_obj_t *hh = label(p, 6, base, h, &font_cn16, C_GOLD);
        one_line(hh, 228);
        base += 22;
    }

    int mi = s_idx_hist[s_sel % s_n_hist];
    const cs_match_t *m = &cs_data()->m[mi];
    char buf[96];

    // 我的队伍是否获胜 → 绿/金
    bool my_t1 = ft && ieq(m->t1_name, ft);
    bool my_t2 = ft && ieq(m->t2_name, ft);
    bool win = (my_t1 && m->score1 > m->score2) || (my_t2 && m->score2 > m->score1);
    bool loss = (my_t1 && m->score1 < m->score2) || (my_t2 && m->score2 < m->score1);
    uint32_t tagc = win ? C_GREEN : (loss ? C_GOLD : C_MUTED);

    snprintf(buf, sizeof(buf), "%s", win ? "胜" : (loss ? "负" : "—"));
    lv_obj_t *tg = label(p, 6, base, buf, &font_cn16, tagc);
    one_line(tg, 40);

    lv_obj_t *ev = label(p, 44, base, m->event, &font_cn16, C_INK);
    one_line(ev, 180);

    int y = base + 20;
    snprintf(buf, sizeof(buf), "%s %d:%d %s",
             my_t1 ? m->t2_name : m->t1_name,
             my_t1 ? m->score2 % 1000 : m->score1 % 1000,
             my_t1 ? m->score1 % 1000 : m->score2 % 1000,
             m->date);
    lv_obj_t *ln = label(p, 6, y, buf, &font_cn16, C_INK);
    one_line(ln, 228);

    y += 22;
    snprintf(buf, sizeof(buf), "%s · %s", m->stage, m->bo);
    lv_obj_t *sb = label(p, 6, y, buf, &font_cn16, C_MUTED);
    one_line(sb, 228);

    // 地图小条
    y += 22;
    for (int i = 0; i < m->map_count && i < CS_MAX_MAPS; i++) {
        const cs_map_t *mp = &m->maps[i];
        panel(p, 6, y, 228, 20, C_PANEL, C_LINE, 6);
        panel(p, 10, y + 4, 3, 12, map_color(mp->name), map_color(mp->name), 0);
        lv_obj_t *nm = label(p, 18, y + 2, mp->cn[0] ? mp->cn : mp->name, &font_cn16, C_INK);
        one_line(nm, 110);
        bool played = (mp->s1 || mp->s2);
        if (played) snprintf(buf, sizeof(buf), "%d - %d", mp->s1 % 1000, mp->s2 % 1000);
        else        scpy(buf, sizeof(buf), "-");
        uint32_t mc = !played ? C_MUTED
                    : (mp->winner == 1 ? C_GREEN : (mp->winner == 2 ? C_GOLD : C_MUTED));
        lv_obj_t *ms = label(p, 132, y + 2, buf, &font_cn16, mc);
        lv_obj_set_width(ms, 96);
        lv_obj_set_style_text_align(ms, LV_TEXT_ALIGN_RIGHT, 0);
        y += 22;
    }
}

// ---------------------------------------------------------------------------
// 屏幕:NiKo 生涯
// ---------------------------------------------------------------------------
static void build_career(lv_obj_t *p)
{
    const cs_niko_t *n = cs_niko();
    cs_avatar_draw(p, 6, 4, 72);

    lv_obj_t *nm = label(p, 86, 8, (n && n->name[0]) ? n->name : "NiKo", &font_cn16, C_GOLD);
    one_line(nm, 148);
    const char *ft = cs_follow_team();
    if (ft && ft[0]) {
        lv_obj_t *tm = label(p, 86, 30, ft, &font_cn16, C_INK);
        one_line(tm, 148);
    }
    if (n && n->role[0]) {
        lv_obj_t *rl = label(p, 86, 52, n->role, &font_cn16, C_MUTED);
        one_line(rl, 148);
    }

    int y = 88;
    struct { const char *k; char v[24]; uint32_t c; } rows[] = {
        { "Rating",  "", C_GOLD }, { "K/D",     "", C_INK },
        { "ADR",     "", C_INK }, { "KAST",    "", C_INK },
        { "Impact",  "", C_INK }, { "出场地图", "", C_INK },
        { "大赛冠军", "", C_INK }, { "MVP",     "", C_INK },
        { "总奖金",  "", C_INK }, { "职业生涯", "", C_MUTED },
    };
    if (n) {
        snprintf(rows[0].v, sizeof(rows[0].v), "%.2f", n->rating);
        snprintf(rows[1].v, sizeof(rows[1].v), "%.2f", n->kd);
        snprintf(rows[2].v, sizeof(rows[2].v), "%.1f", n->adr);
        snprintf(rows[3].v, sizeof(rows[3].v), "%.1f%%", n->kast);
        snprintf(rows[4].v, sizeof(rows[4].v), "%.2f", n->impact);
        snprintf(rows[5].v, sizeof(rows[5].v), "%d", n->maps);
        snprintf(rows[6].v, sizeof(rows[6].v), "%d", n->majors);
        snprintf(rows[7].v, sizeof(rows[7].v), "%d", n->mvp);
        snprintf(rows[8].v, sizeof(rows[8].v), "%d万$", n->earnings);
        snprintf(rows[9].v, sizeof(rows[9].v), "%s", n->years);
    }
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        lv_obj_t *k = label(p, 6, y, rows[i].k, &font_cn16, C_MUTED);
        one_line(k, 90);
        lv_obj_t *v = label(p, 100, y, rows[i].v, &font_cn16, rows[i].c);
        one_line(v, 120);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        y += 18;
        if (y > CONT_H - 4) break;
    }
}

// ---------------------------------------------------------------------------
// 屏幕:设置 / 网络
// ---------------------------------------------------------------------------
static void build_set(lv_obj_t *p)
{
    char buf[96];
    panel(p, 4, 2, SCR_W - 8, 96, C_PANEL, C_LINE, 8);

    const char *ssid = cs_net_ssid();
    char wbuf[80];
    if (cs_net_online())           snprintf(wbuf, sizeof(wbuf), "%s %s", ssid[0] ? ssid : "已连接", cs_net_ip());
    else if (cs_net_state() == CS_NET_CONNECTING) snprintf(wbuf, sizeof(wbuf), "%s 连接中", ssid[0] ? ssid : "Wi-Fi");
    else if (cs_net_state() == CS_NET_FAILED)     snprintf(wbuf, sizeof(wbuf), "失败:%s", cs_net_conn_err());
    else                                         scpy(wbuf, sizeof(wbuf), "未连接");
    lv_obj_t *lk = label(p, 12, 8, "网络", &font_cn16, C_MUTED);
    lv_obj_set_width(lk, 44);
    lv_obj_t *lv2 = label(p, 56, 8, wbuf, &font_cn16, cs_net_online() ? C_GREEN : C_INK);
    one_line(lv2, SCR_W - 64);

    const cs_data_t *d = cs_data();
    snprintf(buf, sizeof(buf), "%d 场 · %s", d->count % 10000, d->updated);
    lv_obj_t *lk2 = label(p, 12, 28, "数据", &font_cn16, C_MUTED);
    lv_obj_set_width(lk2, 44);
    lv_obj_t *lv3 = label(p, 56, 28, buf, &font_cn16, C_INK);
    one_line(lv3, SCR_W - 64);

    cs_diag_t diag = cs_net_diag();
    lv_obj_t *lk3 = label(p, 12, 48, "诊断", &font_cn16, C_MUTED);
    lv_obj_set_width(lk3, 44);
    lv_obj_t *lv4 = label(p, 56, 48, cs_net_diag_text(),
                          &font_cn16, diag == CS_DIAG_NONE ? C_GREEN : C_AMBER);
    one_line(lv4, SCR_W - 64);

    const char *mir = cs_net_mirror_name();
    lv_obj_t *lk5 = label(p, 12, 68, "来源", &font_cn16, C_MUTED);
    lv_obj_set_width(lk5, 44);
    snprintf(buf, sizeof(buf), "%s%s", mir[0] ? "镜像 " : "", mir[0] ? mir : "内置/手机");
    lv_obj_t *lv5 = label(p, 56, 68, buf, &font_cn16, C_MUTED);
    one_line(lv5, SCR_W - 64);

    int y = 104;
    if (s_portal_on) {
        panel(p, 4, y, SCR_W - 8, 44, 0x152A1C, C_GREEN, 8);
        label(p, 12, y + 3, "配网门户已开", &font_cn16, C_GREEN);
        snprintf(buf, sizeof(buf), "热点 %s  网页 %s", cs_portal_ssid(), cs_portal_url());
        label(p, 12, y + 22, buf, &font_cn16, C_INK);
        y += 50;
    }

    const char *items[SET_ITEMS];
    char item0[40];
    scpy(item0, sizeof(item0), s_portal_on ? "关闭配网门户" : "打开配网门户");
    items[0] = item0;
    items[1] = "立即刷新数据";
    items[2] = "清除缓存回示例";

    for (int i = 0; i < SET_ITEMS; i++) {
        int ry = y + i * 28;
        if (ry + 26 > CONT_H) break;
        bool sel = (i == s_set_sel);
        panel(p, 4, ry, SCR_W - 8, 26, sel ? C_BLUE : C_PANEL, sel ? C_BLUE : C_LINE, 8);
        label(p, 14, ry + 4, items[i], &font_cn16, C_INK);
    }
}

// ---------------------------------------------------------------------------
// 渲染分发
// ---------------------------------------------------------------------------
static void render_content(void)
{
    lv_obj_clean(s_content);
    switch (s_view) {
    case V_MATCH:  build_match(s_content);  break;
    case V_HIST:   build_hist(s_content);   break;
    case V_CAREER: build_career(s_content); break;
    case V_SET:    build_set(s_content);    break;
    default: break;
    }
}

static void render_hint(void)
{
    const char *h;
    switch (s_view) {
    case V_MATCH:  h = "上下=换场  长按=设置  双击=刷新"; break;
    case V_HIST:   h = "上下=换场  长按=设置  双击=刷新"; break;
    case V_CAREER: h = "长按=设置  双击=刷新";           break;
    case V_SET:    h = "上下=选择  确定=执行  长按=返回"; break;
    default:       h = ""; break;
    }
    lv_label_set_text(s_hint, h);
}

// ---------------------------------------------------------------------------
// 按键(只改状态)
// ---------------------------------------------------------------------------
static void exec_set(void)
{
    switch (s_set_sel) {
    case 0:
        if (s_portal_on) { cs_portal_stop(); s_portal_on = false; }
        else if (cs_portal_start() == ESP_OK) s_portal_on = true;
        break;
    case 1: cs_net_refresh(true); break;
    case 2: cs_data_clear_cache(); cs_niko_use_builtin(); break;
    }
    s_dirty = true;
}

static void ok_click(void)
{
    if (s_view == V_MATCH && s_n_live > 1) { s_sel = (s_sel + 1) % s_n_live; s_dirty = true; }
    else if (s_view == V_HIST && s_n_hist > 1) { s_sel = (s_sel + 1) % s_n_hist; s_dirty = true; }
    else if (s_view == V_SET) { exec_set(); }
}

void cs_niko_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    s_last_active = esp_timer_get_time();
    if (btn != BSP_BTN_OK && ev != BSP_BTN_PRESS) return;
    if (ev == BSP_BTN_CLICK) {
        if (s_ignore_click) { s_ignore_click = false; return; }
    } else if (ev == BSP_BTN_DOUBLE || ev == BSP_BTN_LONG) {
        s_ignore_click = true;
    }

    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_LONG) {
            s_view = (s_view == V_SET) ? V_MATCH : V_SET;
            s_sel = 0; s_set_sel = 0; s_dirty = true;
            return;
        }
        if (ev == BSP_BTN_DOUBLE) { cs_net_refresh(true); return; }
        if (ev == BSP_BTN_CLICK) ok_click();
        return;
    }

    // 上下键(PRESS 事件)
    int dir = (btn == BSP_BTN_DOWN) ? 1 : -1;
    if (s_view == V_MATCH && s_n_live > 1) { s_sel = (s_sel + dir + s_n_live) % s_n_live; s_dirty = true; }
    else if (s_view == V_HIST && s_n_hist > 1) { s_sel = (s_sel + dir + s_n_hist) % s_n_hist; s_dirty = true; }
    else { s_view = (v_t)((s_view + dir + V_COUNT) % V_COUNT); s_sel = 0; s_dirty = true; }
}

// ---------------------------------------------------------------------------
// 200ms 定时器:网络维护 + 实时刷新 + 省电
// ---------------------------------------------------------------------------
static void on_tick(lv_timer_t *t)
{
    (void)t;
    cs_net_tick();
    refresh_indices();   // 数据/关注战队可能刚变,先重算索引,状态栏才准

    uint32_t rev = cs_data_rev();
    if (rev != s_seen_rev) { s_seen_rev = rev; s_dirty = true; }

    // 实时比分:有进行中比赛且在线时,每 15s 强制刷新
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (cs_net_online() && s_n_live > 0 && (now_s - s_last_refresh) > 15) {
        s_last_refresh = now_s;
        cs_net_refresh(true);
    }

    // 省电:配网或 live 时不睡;否则空闲 60s 暗屏、10min 深度睡眠
    int64_t idle = (esp_timer_get_time() - s_last_active) / 1000;
    if (s_portal_on || s_n_live > 0) {
        bsp_display_backlight(100);
    } else if (idle > 600000) {
        ESP_LOGI(TAG, "空闲 10 分钟,进入深度睡眠(定时 + 按键唤醒)");
        // 定时 5 分钟自动醒来刷一次比分,不必等用户按键。
        esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL);
        // 三键共用 GPIO0(ADC 分压):松开被上拉到 3.3V=高,任一键按下即被拉低。
        // 用 gpio_config 显式设为输入,避免唤醒前引脚悬空。
        gpio_config_t gc = {
            .pin_bit_mask = (1ULL << NK_BTN_GPIO),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&gc);
        // ESP32-C3 无 EXT0/EXT1(SOC_PM_SUPPORT_EXT0_WAKEUP=0),只能走
        // SOC_GPIO_SUPPORT_DEEPSLEEP_WAKEUP 的 GPIO 唤醒;C3 仅支持 GPIO0~GPIO5。
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

    // 顶栏左:状态/页码;中:网络;右:时间;最右:电量。
    // 中文 16px 字宽约 16px,「全部1/4」约 56px,故左栏给 64px 够用。
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
