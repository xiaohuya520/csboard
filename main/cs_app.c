// cs_app.c —— 看板界面(深色电竞风)+ 状态驱动的按键机。
#include "cs_app.h"
#include "cs_data.h"
#include "cs_fonts.h"
#include "cs_logos.h"
#include "cs_net.h"
#include "cs_portal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "cs_app";

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
#define HINT_H   24
#define CONT_H   (SCR_H - BAR_H - HINT_H)

// ---------------------------------------------------------------------------
// 视图
// ---------------------------------------------------------------------------
typedef enum {
    V_LIVE = 0, V_HIST, V_SOON, V_NET, V_WIFI, V_PASS, V_COUNT
} view_t;

static const char *VIEW_NAME[V_COUNT] = { "实时", "战绩", "预告", "网络", "Wi-Fi", "密码" };

#define NET_ITEMS 4

// ---------------------------------------------------------------------------
// 状态 —— 按键只修改这里,渲染定时器按状态重建界面
// ---------------------------------------------------------------------------
static int      s_view = V_LIVE;
static int      s_sel;             // 内容页:第几场;列表页:第几项
static int      s_char_idx;        // 密码页字符环下标
static char     s_pass[64];
static int      s_pass_len;
static char     s_pass_ssid[64];
static bool     s_pass_secure;
static bool     s_ignore_click;    // 吞掉双击/长按后驱动补报的那一次 CLICK(按次数,与时间无关)
static bool     s_want_refresh;
static bool     s_dirty = true;
static int      s_tick;
static bool     s_portal_on;

static cs_net_state_t   s_prev_state;
static cs_fetch_state_t s_prev_fetch;
static bool             s_prev_scanbusy;
static uint32_t         s_seen_rev = 0xFFFFFFFFu;

static lv_obj_t *s_scr;
static lv_obj_t *s_bar_l, *s_bar_c, *s_bar_r, *s_bar_line;
static lv_obj_t *s_content;
static lv_obj_t *s_hint;

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
    lv_obj_set_style_border_color(o, lv_color_hex(br), 0);
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

// 单行省略号标签
static void one_line(lv_obj_t *l, int w)
{
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
}

// 队标:找不到就用队色徽章顶替,绝不空着
static void logo(lv_obj_t *p, int x, int y, const char *id, const char *name,
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
    panel(p, x, y, edge, edge, color, color, small ? 3 : 6);
    lv_obj_t *bx = lv_obj_create(p);
    lv_obj_remove_flag(bx, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bx, x, y);
    lv_obj_set_size(bx, edge, edge);
    lv_obj_set_style_bg_opa(bx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bx, 0, 0);
    lv_obj_set_style_pad_all(bx, 0, 0);
    char t[6];
    snprintf(t, sizeof(t), "%.3s", (name && name[0]) ? name : "?");
    lv_obj_t *lb = label(bx, 0, small ? 2 : 16, t, small ? &lv_font_montserrat_14 : &font_cn16, 0xFFFFFF);
    lv_obj_set_width(lb, edge);
    lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
}

static uint32_t map_color(const char *en)
{
    static const struct { const char *n; uint32_t c; } M[] = {
        { "Mirage",   0xC7A76A }, { "Inferno",  0xD9632F }, { "Nuke",     0x7FB069 },
        { "Dust2",    0xD8B36A }, { "Ancient",  0x3E9E8F }, { "Anubis",   0x2F81F7 },
        { "Overpass", 0x6B8E23 }, { "Vertigo",  0x8C8C99 }, { "Train",    0x9AA5B1 },
        { "Dust",     0xD8B36A },
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
// 当前页的筛选与取数
// ---------------------------------------------------------------------------
static const char *cur_filter(void)
{
    switch (s_view) {
    case V_LIVE: return CS_ST_LIVE;
    case V_HIST: return CS_ST_FINISHED;
    case V_SOON: return CS_ST_UPCOMING;
    default:     return NULL;
    }
}

static int cur_count(void)
{
    const char *f = cur_filter();
    const cs_data_t *d = cs_data();
    if (!f) return 0;
    int n = 0;
    for (int i = 0; i < d->count; i++)
        if (strcmp(d->m[i].status, f) == 0) n++;
    return n;
}

static int cur_match(int nth)
{
    const char *f = cur_filter();
    const cs_data_t *d = cs_data();
    if (!f) return -1;
    int k = 0;
    for (int i = 0; i < d->count; i++) {
        if (strcmp(d->m[i].status, f) != 0) continue;
        if (k == nth) return i;
        k++;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 渲染:状态栏
// ---------------------------------------------------------------------------
static void render_status(void)
{
    char buf[80];

    if (s_view <= V_SOON) {
        int n = cur_count();
        int pos = n ? s_sel + 1 : 0;
        snprintf(buf, sizeof(buf), "%s %d/%d", VIEW_NAME[s_view], pos, n);
    } else {
        snprintf(buf, sizeof(buf), "%s", VIEW_NAME[s_view]);
    }
    lv_label_set_text(s_bar_l, buf);

    const char *src = cs_data_source();
    const char *srcn = ieq(src, "net") ? "网络"
                     : ieq(src, "cache") ? "缓存"
                     : ieq(src, "手机") ? "手机" : "示例";
    const char *pssid = cs_portal_ssid();
    if (cs_portal_active()) {
        snprintf(buf, sizeof(buf), "门户 %s", pssid);
    } else if (cs_net_online()) {
        snprintf(buf, sizeof(buf), "在线 %s", srcn);
    } else if (cs_net_state() == CS_NET_CONNECTING) {
        scpy(buf, sizeof(buf), "连接中");
    } else if (cs_net_state() == CS_NET_SCANNING) {
        scpy(buf, sizeof(buf), "扫描中");
    } else {
        snprintf(buf, sizeof(buf), "离线 %s", srcn);
    }
    lv_label_set_text(s_bar_c, buf);

    char tm[8];
    cs_time_hhmm(tm, sizeof(tm));
    lv_label_set_text(s_bar_r, tm);

    uint32_t lc = C_MUTED;
    cs_fetch_state_t fs = cs_data_fetch_state();
    if (fs == CS_FETCH_RUNNING)      lc = C_BLUE;
    else if (fs == CS_FETCH_FAIL)    lc = C_RED;
    else if (cs_net_online())        lc = C_GREEN;
    lv_obj_set_style_bg_color(s_bar_line, lv_color_hex(lc), 0);
}

// ---------------------------------------------------------------------------
// 渲染:比赛卡
// ---------------------------------------------------------------------------
static void build_card(lv_obj_t *p, const cs_match_t *m)
{
    uint32_t sc = status_color(m->status);
    panel(p, 0, 0, 4, CONT_H, sc, sc, 0);          // 左侧状态色条

    char buf[128];

    // 赛事名(单行省略)+ 阶段/时间/赛制
    lv_obj_t *ev = label(p, 8, 4, m->event, &font_cn16, C_INK);
    one_line(ev, SCR_W - 16);
    lv_obj_set_style_text_align(ev, LV_TEXT_ALIGN_CENTER, 0);

    if (m->stage[0]) snprintf(buf, sizeof(buf), "%s · %s %s · %s", m->stage, m->date, m->time, m->bo);
    else             snprintf(buf, sizeof(buf), "%s %s · %s", m->date, m->time, m->bo);
    lv_obj_t *sub = label(p, 8, 26, buf, &font_cn16, C_MUTED);
    one_line(sub, SCR_W - 16);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    // 队名
    lv_obj_t *n1 = label(p, 8, 52, m->t1_name, &font_cn16, C_INK);
    lv_obj_set_width(n1, 104);
    one_line(n1, 104);

    lv_obj_t *n2 = label(p, SCR_W - 112, 52, m->t2_name, &font_cn16, C_INK);
    lv_obj_set_width(n2, 104);
    lv_obj_set_style_text_align(n2, LV_TEXT_ALIGN_RIGHT, 0);
    one_line(n2, 104);

    // 队标
    logo(p, 8, 74, m->t1_logo, m->t1_name, false, m->t1_color);
    logo(p, SCR_W - 56, 74, m->t2_logo, m->t2_name, false, m->t2_color);

    // 比分
    snprintf(buf, sizeof(buf), "%d : %d", m->score1 % 1000, m->score2 % 1000);
    uint32_t score_c = ieq(m->status, CS_ST_FINISHED)
                     ? (m->score1 > m->score2 ? C_GREEN : (m->score2 > m->score1 ? C_GOLD : C_MUTED))
                     : C_INK;
    lv_obj_t *sc_l = label(p, 56, 82, buf, &lv_font_montserrat_28, score_c);
    lv_obj_set_width(sc_l, 128);
    lv_obj_set_style_text_align(sc_l, LV_TEXT_ALIGN_CENTER, 0);

    // 状态
    lv_obj_t *st = label(p, 8, 124, status_cn(m->status), &font_cn16, sc);
    one_line(st, SCR_W - 16);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);

    // 地图行
    if (m->map_count <= 0) {
        const char *tip = ieq(m->status, CS_ST_UPCOMING) ? "比赛尚未开始" : "暂无地图数据";
        lv_obj_t *t2 = label(p, 8, 168, tip, &font_cn16, C_MUTED);
        one_line(t2, SCR_W - 16);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int y = 148;
    for (int i = 0; i < m->map_count && i < CS_MAX_MAPS; i++) {
        const cs_map_t *mp = &m->maps[i];
        panel(p, 6, y, SCR_W - 12, 22, C_PANEL, C_LINE, 6);
        panel(p, 10, y + 5, 3, 12, map_color(mp->name), map_color(mp->name), 0);

        lv_obj_t *nm = label(p, 18, y + 3, mp->cn[0] ? mp->cn : mp->name, &font_cn16, C_INK);
        one_line(nm, 110);

        bool played = (mp->s1 || mp->s2);
        if (played) snprintf(buf, sizeof(buf), "%d - %d", mp->s1 % 1000, mp->s2 % 1000);
        else        scpy(buf, sizeof(buf), "-");
        uint32_t mc = !played ? C_MUTED
                    : (mp->winner == 1 ? C_GREEN : (mp->winner == 2 ? C_GOLD : C_MUTED));
        lv_obj_t *ms = label(p, 132, y + 3, buf, &font_cn16, mc);
        lv_obj_set_width(ms, SCR_W - 142);
        lv_obj_set_style_text_align(ms, LV_TEXT_ALIGN_RIGHT, 0);
        y += 24;
    }
}

static void build_center_msg(lv_obj_t *p, const char *msg, uint32_t color)
{
    lv_obj_t *l = label(p, 8, 116, msg, &font_cn16, color);
    one_line(l, SCR_W - 16);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------
// 渲染:网络页
// ---------------------------------------------------------------------------
static void kv_row(lv_obj_t *p, int x, int y, int w, const char *k, const char *v, uint32_t vc)
{
    lv_obj_t *lk = label(p, x, y, k, &font_cn16, C_MUTED);
    lv_obj_set_width(lk, 44);
    lv_obj_t *lv2 = label(p, x + 44, y, v, &font_cn16, vc);
    one_line(lv2, w - 44);
}

static void build_net(lv_obj_t *p)
{
    char buf[96];

    panel(p, 4, 2, SCR_W - 8, 104, C_PANEL, C_LINE, 8);

    const char *ssid = cs_net_ssid();
    char wbuf[80];
    if (cs_net_online())      snprintf(wbuf, sizeof(wbuf), "%s %s", ssid[0] ? ssid : "已连接", cs_net_ip());
    else if (cs_net_state() == CS_NET_CONNECTING) snprintf(wbuf, sizeof(wbuf), "%s 连接中", ssid[0] ? ssid : "Wi-Fi");
    else if (cs_net_state() == CS_NET_FAILED)     snprintf(wbuf, sizeof(wbuf), "失败:%s", cs_net_conn_err());
    else                                          scpy(wbuf, sizeof(wbuf), "未连接");
    kv_row(p, 12, 8, SCR_W - 24, "网络", wbuf, cs_net_online() ? C_GREEN : C_INK);

    const cs_data_t *d = cs_data();
    snprintf(buf, sizeof(buf), "%d 场 · %s", d->count % 10000, d->updated);
    kv_row(p, 12, 28, SCR_W - 24, "数据", buf, C_INK);

    cs_fetch_state_t fs = cs_data_fetch_state();
    uint32_t fc = (fs == CS_FETCH_OK) ? C_GREEN : (fs == CS_FETCH_FAIL ? C_RED : C_MUTED);
    kv_row(p, 12, 48, SCR_W - 24, "拉取", cs_data_fetch_msg(), fc);

    const char *mir = cs_net_mirror_name();
    kv_row(p, 12, 68, SCR_W - 24, "诊断", cs_net_diag_text(),
           cs_net_diag() == CS_DIAG_NONE ? C_GREEN : C_AMBER);

    if (mir[0]) snprintf(buf, sizeof(buf), "镜像 %s", mir);
    else        scpy(buf, sizeof(buf), "手机投递 / 内置");
    kv_row(p, 12, 88, SCR_W - 24, "来源", buf, C_MUTED);

    int my = 106;
    if (s_portal_on) {
        panel(p, 4, my, SCR_W - 8, 48, 0x152A1C, C_GREEN, 8);
        label(p, 12, my + 3, "配网门户已开", &font_cn16, C_GREEN);
        snprintf(buf, sizeof(buf), "%s", cs_portal_ssid());
        kv_row(p, 12, my + 20, SCR_W - 24, "热点", buf, C_INK);
        snprintf(buf, sizeof(buf), "密码 %s %s", cs_portal_pass(), cs_portal_url());
        kv_row(p, 12, my + 36, SCR_W - 24, "网页", buf, C_INK);
        my += 54;
    }

    const char *items[NET_ITEMS];
    char item0[40];
    scpy(item0, sizeof(item0), s_portal_on ? "关闭配网门户" : "打开配网门户");
    items[0] = item0;
    items[1] = "选择 Wi-Fi 网络";
    items[2] = "立即拉取数据";
    items[3] = "清除数据缓存";

    for (int i = 0; i < NET_ITEMS; i++) {
        int y = my + i * 28;
        if (y + 26 > CONT_H) break;
        bool sel = (i == s_sel);
        panel(p, 4, y, SCR_W - 8, 26, sel ? C_BLUE : C_PANEL, sel ? C_BLUE : C_LINE, 8);
        lv_obj_t *l = label(p, 14, y + 4, items[i], &font_cn16, C_INK);
        one_line(l, SCR_W - 28);
    }
}

// ---------------------------------------------------------------------------
// 渲染:Wi-Fi 列表
// ---------------------------------------------------------------------------
static void build_wifi(lv_obj_t *p)
{
    char buf[64];
    int n = cs_net_ap_count();

    if (cs_net_scan_busy())      scpy(buf, sizeof(buf), "正在扫描…");
    else if (n <= 0)             scpy(buf, sizeof(buf), "没有扫到网络,按确定重试");
    else                         snprintf(buf, sizeof(buf), "找到 %d 个网络,上下选择", n);
    lv_obj_t *h = label(p, 6, 4, buf, &font_cn16, C_MUTED);
    one_line(h, SCR_W - 12);

    int maxrow = 8;
    for (int i = 0; i < n && i < maxrow; i++) {
        int y = 28 + i * 30;
        bool sel = (i == s_sel);
        panel(p, 4, y, SCR_W - 8, 26, sel ? C_BLUE : C_PANEL, sel ? C_BLUE : C_LINE, 8);

        const char *ap = cs_net_ap_ssid(i);
        const char *prev = cs_net_ap_prev_ssid();
        bool is_prev = prev[0] && ieq(prev, ap);
        snprintf(buf, sizeof(buf), "%s%s", is_prev ? "已连 " : "", ap);
        lv_obj_t *nm = label(p, 12, y + 4, buf, &font_cn16, is_prev ? C_GOLD : C_INK);
        one_line(nm, SCR_W - 76);

        snprintf(buf, sizeof(buf), "%s %d", cs_net_ap_secure(i) ? "密" : "开", cs_net_ap_rssi(i));
        lv_obj_t *rs = label(p, SCR_W - 64, y + 4, buf, &font_cn16, C_MUTED);
        lv_obj_set_width(rs, 56);
        lv_obj_set_style_text_align(rs, LV_TEXT_ALIGN_RIGHT, 0);
    }

    const char *msg = cs_net_scan_msg();
    if (msg[0]) {
        lv_obj_t *m = label(p, 6, CONT_H - 22, msg, &font_cn16, C_AMBER);
        one_line(m, SCR_W - 12);
    }
}

// ---------------------------------------------------------------------------
// 渲染:密码页
// ---------------------------------------------------------------------------
static const char RING[] = "abcdefghijklmnopqrstuvwxyz0123456789-_.@#";
#define RING_N    ((int)(sizeof(RING) - 1))
#define PASS_ITEMS (RING_N + 2)          // 末尾两项:退格 / 完成

static void build_pass(lv_obj_t *p)
{
    char buf[96];

    snprintf(buf, sizeof(buf), "连接 %s", s_pass_ssid);
    lv_obj_t *t = label(p, 6, 4, buf, &font_cn16, C_INK);
    one_line(t, SCR_W - 12);

    if (s_pass_len) snprintf(buf, sizeof(buf), "密码: %s", s_pass);
    else            scpy(buf, sizeof(buf), "密码: (空)");
    lv_obj_t *pw = label(p, 6, 30, buf, &font_cn16, C_GOLD);
    one_line(pw, SCR_W - 12);

    panel(p, 20, 62, SCR_W - 40, 68, C_PANEL, C_BLUE, 10);
    if (s_char_idx < RING_N) {
        char c[4];
        c[0] = RING[s_char_idx];
        c[1] = 0;
        lv_obj_t *big = label(p, 20, 76, c, &lv_font_montserrat_28, C_INK);
        lv_obj_set_width(big, SCR_W - 40);
        lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *hint = label(p, 20, 110, "上下=换字符  确定=输入", &font_cn16, C_MUTED);
        lv_obj_set_width(hint, SCR_W - 40);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        const char *act = (s_char_idx == RING_N) ? "退格" : "完成";
        lv_obj_t *big = label(p, 20, 82, act, &font_cn16, s_char_idx == RING_N ? C_AMBER : C_GREEN);
        lv_obj_set_width(big, SCR_W - 40);
        lv_obj_set_style_text_align(big, LV_TEXT_ALIGN_CENTER, 0);
    }

    lv_obj_t *h1 = label(p, 6, 140, s_pass_secure ? "此网络需要密码" : "此网络开放,无需密码", &font_cn16, C_MUTED);
    one_line(h1, SCR_W - 12);
    lv_obj_t *h2 = label(p, 6, 160, "上下切换字符,确定输入", &font_cn16, C_MUTED);
    one_line(h2, SCR_W - 12);
    lv_obj_t *h3 = label(p, 6, 180, "双击=退格,长按确定=连接", &font_cn16, C_MUTED);
    one_line(h3, SCR_W - 12);
    lv_obj_t *h4 = label(p, 6, 200, "大写/复杂密码请用手机门户", &font_cn16, C_AMBER);
    one_line(h4, SCR_W - 12);
}

// ---------------------------------------------------------------------------
// 渲染:分发
// ---------------------------------------------------------------------------
static void render_content(void)
{
    lv_obj_clean(s_content);

    switch (s_view) {
    case V_LIVE:
    case V_HIST:
    case V_SOON: {
        int n = cur_count();
        if (n <= 0) { build_center_msg(s_content, "暂无赛事", C_MUTED); break; }
        if (s_sel >= n) s_sel = n - 1;
        if (s_sel < 0)  s_sel = 0;
        int mi = cur_match(s_sel);
        if (mi >= 0) build_card(s_content, &cs_data()->m[mi]);
        break;
    }
    case V_NET:  build_net(s_content);  break;
    case V_WIFI: build_wifi(s_content); break;
    case V_PASS: build_pass(s_content); break;
    default: break;
    }
}

static void render_hint(void)
{
    const char *h;
    switch (s_view) {
    case V_LIVE:
    case V_HIST:
    case V_SOON: h = "上下=换场  长按=网络  双击=换页"; break;
    case V_NET:  h = "上下=选择  确定=执行  长按=返回";  break;
    case V_WIFI: h = "上下=选网  确定=连接  长按=返回";  break;
    case V_PASS: h = "上下=选字符  确定=输入  长按=连接"; break;
    default:     h = ""; break;
    }
    lv_label_set_text(s_hint, h);
}

// ---------------------------------------------------------------------------
// 按键:只改状态
// ---------------------------------------------------------------------------
static void key_content(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    int n = cur_count();
    if (btn == BSP_BTN_UP) {
        if (n > 0) s_sel = (s_sel + n - 1) % n;
    } else if (btn == BSP_BTN_DOWN) {
        if (n > 0) s_sel = (s_sel + 1) % n;
    } else if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            s_want_refresh = true;                 // 幂等动作:多一次也只会多刷一次数据
        } else if (ev == BSP_BTN_DOUBLE) {
            s_view = (view_t)((s_view + 1) % 3);   // 实时 -> 战绩 -> 预告
            s_sel = 0;
        } else if (ev == BSP_BTN_LONG) {
            s_view = V_NET;
            s_sel = 0;
        }
    }
}

static void key_net(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_UP) {
        s_sel = (s_sel + NET_ITEMS - 1) % NET_ITEMS;
    } else if (btn == BSP_BTN_DOWN) {
        s_sel = (s_sel + 1) % NET_ITEMS;
    } else if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            switch (s_sel) {
            case 0:
                if (s_portal_on) { cs_portal_stop(); s_portal_on = false; }
                else if (cs_portal_start() == ESP_OK) s_portal_on = true;
                break;
            case 1:
                s_view = V_WIFI;
                s_sel = 0;
                cs_net_scan();
                break;
            case 2:
                s_want_refresh = true;
                break;
            default:
                cs_data_clear_cache();
                cs_data_use_builtin();
                break;
            }
        } else if (ev == BSP_BTN_LONG || ev == BSP_BTN_DOUBLE) {
            s_view = V_LIVE;
            s_sel = 0;
        }
    }
}

static void key_wifi(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    int n = cs_net_ap_count();
    if (btn == BSP_BTN_UP) {
        if (n > 0) s_sel = (s_sel + n - 1) % n;
    } else if (btn == BSP_BTN_DOWN) {
        if (n > 0) s_sel = (s_sel + 1) % n;
    } else if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            if (cs_net_scan_busy()) return;
            if (n <= 0) { cs_net_scan(); return; }
            const char *ssid = cs_net_ap_ssid(s_sel);
            if (!ssid[0]) return;
            scpy(s_pass_ssid, sizeof(s_pass_ssid), ssid);
            s_pass_secure = cs_net_ap_secure(s_sel);
            if (!s_pass_secure) {
                cs_net_connect(ssid, "");
                s_view = V_NET;
                s_sel = 0;
            } else {
                s_pass[0] = 0;
                s_pass_len = 0;
                s_char_idx = 0;
                s_view = V_PASS;
            }
        } else if (ev == BSP_BTN_LONG || ev == BSP_BTN_DOUBLE) {
            s_view = V_NET;
            s_sel = 0;
        }
    }
}

static void key_pass(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (btn == BSP_BTN_UP) {
        s_char_idx = (s_char_idx + PASS_ITEMS - 1) % PASS_ITEMS;
    } else if (btn == BSP_BTN_DOWN) {
        s_char_idx = (s_char_idx + 1) % PASS_ITEMS;
    } else if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            if (s_char_idx < RING_N) {
                if (s_pass_len < (int)sizeof(s_pass) - 1) {
                    s_pass[s_pass_len++] = RING[s_char_idx];
                    s_pass[s_pass_len] = 0;
                }
            } else if (s_char_idx == RING_N) {
                if (s_pass_len > 0) s_pass[--s_pass_len] = 0;
            } else {
                cs_net_connect(s_pass_ssid, s_pass);
                s_view = V_NET;
                s_sel = 0;
            }
        } else if (ev == BSP_BTN_DOUBLE) {
            if (s_pass_len > 0) s_pass[--s_pass_len] = 0;
        } else if (ev == BSP_BTN_LONG) {
            cs_net_connect(s_pass_ssid, s_pass);
            s_view = V_NET;
            s_sel = 0;
        }
    }
}

void cs_app_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    // 双击/长按之后驱动可能补报一次 CLICK:按"次数"精确吞掉那一颗。
    // 就算漏吞了也无所谓 —— 单击只会触发一次幂等刷新,不会改变页面。
    if (ev == BSP_BTN_CLICK) {
        if (s_ignore_click) { s_ignore_click = false; return; }
    } else if (ev == BSP_BTN_DOUBLE || ev == BSP_BTN_LONG) {
        s_ignore_click = true;
    }

    switch (s_view) {
    case V_LIVE:
    case V_HIST:
    case V_SOON: key_content(btn, ev); break;
    case V_NET:  key_net(btn, ev);     break;
    case V_WIFI: key_wifi(btn, ev);    break;
    case V_PASS: key_pass(btn, ev);    break;
    default: break;
    }

    s_dirty = true;
}

// ---------------------------------------------------------------------------
// 定时器:驱动网络维护 + 按状态重建界面
// ---------------------------------------------------------------------------
static void on_tick(lv_timer_t *t)
{
    (void)t;
    cs_net_tick();

    if (s_want_refresh) {
        s_want_refresh = false;
        cs_net_refresh(true);
    }

    uint32_t rev = cs_data_rev();
    if (rev != s_seen_rev) { s_seen_rev = rev; s_dirty = true; }

    cs_net_state_t st = cs_net_state();
    if (st != s_prev_state) { s_prev_state = st; s_dirty = true; }

    cs_fetch_state_t fs = cs_data_fetch_state();
    if (fs != s_prev_fetch) { s_prev_fetch = fs; s_dirty = true; }

    bool sb = cs_net_scan_busy();
    if (sb != s_prev_scanbusy) { s_prev_scanbusy = sb; s_dirty = true; }

    bool pv = cs_portal_active();
    if (pv != s_portal_on) { s_portal_on = pv; s_dirty = true; }

    render_status();
    if (s_dirty) {
        s_dirty = false;
        render_content();
        render_hint();
    }

    // 网络页里"拉取中"这类文案需要随进度刷新(状态变化已覆盖,这里兜一层节拍)
    if (++s_tick >= 10) {
        s_tick = 0;
        if (s_view == V_NET || s_view == V_WIFI) { render_content(); }
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
void cs_app_start(void)
{
    cs_logos_init();

    s_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(s_scr, 0, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);

    s_bar_l = label(s_scr, 6, 4, "", &font_cn16, C_INK);
    lv_obj_set_width(s_bar_l, 66);
    one_line(s_bar_l, 66);

    s_bar_c = label(s_scr, 74, 4, "", &font_cn16, C_MUTED);
    lv_obj_set_width(s_bar_c, 108);
    one_line(s_bar_c, 108);

    s_bar_r = label(s_scr, 186, 5, "", &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(s_bar_r, 48);
    lv_obj_set_style_text_align(s_bar_r, LV_TEXT_ALIGN_RIGHT, 0);

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

    s_prev_state   = cs_net_state();
    s_prev_fetch   = cs_data_fetch_state();
    s_prev_scanbusy = cs_net_scan_busy();
    s_seen_rev     = cs_data_rev();

    render_status();
    render_content();
    render_hint();

    lv_timer_create(on_tick, 200, NULL);
    lv_screen_load(s_scr);

    ESP_LOGI(TAG, "看板界面就绪");
}
