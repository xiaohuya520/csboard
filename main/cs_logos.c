// cs_logos.c -- team logo lookup.
//
// Two layers, checked in order:
//   1) compile-time embedded bitmaps (cs_logo_data.c, 105 teams, always present)
//   2) runtime resource pack in the dedicated "csres" flash partition
//      (up to 200 extra teams fetched by CI from bo3.gg's world ranking).
//
// The pack is read on demand into small pooled buffers: a 4MB mmap would blow
// the C3's MMU page budget, and reading 9KB per logo only happens on page
// changes, so esp_partition_read into a round-robin pool is the right trade.
// The pack is disabled entirely (zero RAM, embedded-only) if the partition is
// absent, the magic/CRC does not verify, or the pool cannot be allocated.
#include "cs_logos.h"
#include "cs_fonts.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_crc.h"

#define LOGO_N       105
#define EDGE48       48
#define EDGE20       20
#define PIX48        (EDGE48 * EDGE48)
#define PIX20        (EDGE20 * EDGE20)

// Generated in cs_logo_data.c
extern const uint16_t cs_logo48[LOGO_N * PIX48];
extern const uint16_t cs_logo20[LOGO_N * PIX20];

typedef struct {
    const char *id;
    uint16_t    idx;
} logo_ent_t;

// Lower-case ids plus common aliases. Match is case-insensitive.
static const logo_ent_t IDS[] = {
    { "3dmax", 0 },
    { "9pandas", 1 },
    { "9-pandas", 1 },
    { "9z", 2 },
    { "amkal", 3 },
    { "apeks", 4 },
    { "apeks-rebels", 5 },
    { "astralis", 6 },
    { "astralis-talent", 7 },
    { "atk", 8 },
    { "attax", 9 },
    { "alternate-attax", 9 },
    { "aurora", 10 },
    { "b8", 11 },
    { "betboom", 12 },
    { "big", 13 },
    { "big-clan", 13 },
    { "bleed", 14 },
    { "bne", 15 },
    { "bad-news-eagles", 15 },
    { "cloud9", 16 },
    { "complexity", 17 },
    { "cphflames", 18 },
    { "copenhagen-flames", 18 },
    { "dignitas", 19 },
    { "ecstatic", 20 },
    { "eg", 21 },
    { "evil-geniuses", 21 },
    { "ence", 22 },
    { "endpoint", 23 },
    { "entropiq", 24 },
    { "eternalfire", 25 },
    { "eternal-fire", 25 },
    { "ex-anonymo", 26 },
    { "anonymo", 26 },
    { "exfinest", 27 },
    { "finest", 27 },
    { "extremum", 28 },
    { "falcons", 29 },
    { "faze", 30 },
    { "faze-clan", 30 },
    { "fluxo", 31 },
    { "fnatic", 32 },
    { "fnatic-rising", 33 },
    { "forze", 34 },
    { "furia", 35 },
    { "g2", 36 },
    { "g2-esports", 36 },
    { "gaimin", 37 },
    { "gaimin-gladiators", 37 },
    { "gamerlegion", 38 },
    { "team-gamerlegion", 38 },
    { "godsent", 39 },
    { "grayhound", 40 },
    { "gtz", 41 },
    { "havu", 42 },
    { "hellraiser", 43 },
    { "hellraisers", 43 },
    { "heroic", 44 },
    { "illuminar", 45 },
    { "imperial", 46 },
    { "insilio", 47 },
    { "isurus", 48 },
    { "itb", 49 },
    { "into-the-breach", 49 },
    { "jano", 50 },
    { "k23", 51 },
    { "koi", 52 },
    { "legacy", 53 },
    { "legion", 54 },
    { "hard-legion", 54 },
    { "liquid", 55 },
    { "team-liquid", 55 },
    { "lynnvision", 56 },
    { "lynn-vision", 56 },
    { "m80", 57 },
    { "madlions", 58 },
    { "mad-lions", 58 },
    { "majestic", 59 },
    { "majestic-lions", 59 },
    { "metizport", 60 },
    { "mibr", 61 },
    { "mongolz", 62 },
    { "the-mongolz", 62 },
    { "monte", 63 },
    { "mouz", 64 },
    { "mouz-esports", 64 },
    { "mouz-nxt", 65 },
    { "movistar", 66 },
    { "movistar-riders", 66 },
    { "navi", 67 },
    { "natus-vincere", 67 },
    { "na-vi", 67 },
    { "navijunior", 68 },
    { "navi-junior", 68 },
    { "nemiga", 69 },
    { "nip", 70 },
    { "ninjas-in-pyjamas", 70 },
    { "nordavind", 71 },
    { "nouns", 72 },
    { "nrg", 73 },
    { "oddik", 74 },
    { "og", 75 },
    { "order", 76 },
    { "pain", 77 },
    { "pain-gaming", 77 },
    { "parivision", 78 },
    { "partizan", 79 },
    { "passionua", 80 },
    { "passion-ua", 80 },
    { "rareatom", 81 },
    { "rare-atom", 81 },
    { "rebels", 82 },
    { "redcanids", 83 },
    { "red-canids", 83 },
    { "renegades", 84 },
    { "rooster", 85 },
    { "sangal", 86 },
    { "sashi", 87 },
    { "sharks", 88 },
    { "sinners", 89 },
    { "skade", 90 },
    { "spirit", 91 },
    { "team-spirit", 91 },
    { "spiritacad", 92 },
    { "spirit-academy", 92 },
    { "sprout", 93 },
    { "tyloo", 94 },
    { "unicorns", 95 },
    { "unicorns-of-love", 95 },
    { "vertex", 96 },
    { "virtuspro", 97 },
    { "virtus-pro", 97 },
    { "vp", 97 },
    { "virtuspro-acad", 98 },
    { "vp.prodigy", 98 },
    { "vitality", 99 },
    { "wildcard", 100 },
    { "wingsup", 101 },
    { "wings-up", 101 },
    { "winstrike", 102 },
    { "young-ninjas", 103 },
    { "youngsters", 104 },
    { "saw-youngsters", 104 },
};

// BSS: descriptors only (105 x ~48B). Pixels stay in flash.
static lv_image_dsc_t s_dsc48[LOGO_N];
static lv_image_dsc_t s_dsc20[LOGO_N];
static bool          s_ready;

// ---------------- resource pack ("csres" partition) ----------------
//
// 64B header, little-endian:
//   [0] magic "CSRP" | [1] version=1 | [2] slot_count | [3] alias_count
//   [4] alias_off    | [5] pix48_off | [6] pix20_off    | [7] crc32(body)
//   [8] total_size   | rest reserved (0)
// 32B alias entry: char id[24] | u16 slot | u16 rsvd | u32 color | u32 rsvd
// Pixel blocks: slot_count * 48*48*2 then slot_count * 20*20*2 (RGB565).
#define PACK_HDR_SIZE   64
#define PACK_ALIAS_SIZE 32
#define PACK_MAX_SLOTS  256
#define PACK_MAX_ALIAS  480
#define POOL48_N        3
#define POOL20_N        2

typedef struct {
    char     id[24];
    uint16_t slot;
    uint16_t rsvd;
    uint32_t color;
    uint32_t rsvd2;
} pack_alias_t;

static const esp_partition_t *s_res;
static uint32_t s_slots, s_alias_n, s_alias_off, s_p48_off, s_p20_off;
static pack_alias_t *s_alias;              // heap, only when pack is valid

// Pools: LVGL image descriptors reference these buffers, so a buffer must stay
// valid while its lv_image lives on screen. 3x48 + 2x20 covers every screen
// (versus card = 2x48 + 1x20 at most); round-robin evicts the oldest.
static uint16_t      *s_buf48[POOL48_N];
static lv_image_dsc_t s_pdsc48[POOL48_N];
static char           s_pid48[POOL48_N][24];
static int            s_pcur48;

static uint16_t      *s_buf20[POOL20_N];
static lv_image_dsc_t s_pdsc20[POOL20_N];
static char           s_pid20[POOL20_N][24];
static int            s_pcur20;

static bool id_eq(const char *a, const char *b)
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

static int find_idx(const char *id)
{
    if (!id || !id[0]) return -1;
    for (unsigned i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++)
        if (id_eq(IDS[i].id, id)) return (int)IDS[i].idx;
    return -1;
}

// 归一化:小写 + 去掉非字母数字(与生成端的 norm() 一致),
// 让 "The MongolZ" / "the-mongolz" / "themongolz" 都能命中同一条。
static void norm_id(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    if (in) {
        for (const char *p = in; *p && n + 1 < cap; p++) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[n++] = c;
        }
    }
    out[n] = 0;
}

static uint32_t u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 校验失败/内存不足时的整体回退:释放所有包资源,回到纯内嵌模式。
static void pack_disable(void)
{
    free(s_alias);
    s_alias = NULL;
    for (int i = 0; i < POOL48_N; i++) { free(s_buf48[i]); s_buf48[i] = NULL; }
    for (int i = 0; i < POOL20_N; i++) { free(s_buf20[i]); s_buf20[i] = NULL; }
    s_res = NULL;
    s_slots = s_alias_n = 0;
}

// 校验并登记资源包。任何一步不对都退回内嵌层,绝不带病上岗。
static void pack_probe(void)
{
    s_res = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY, "csres");
    if (!s_res || s_res->size < PACK_HDR_SIZE) return;

    uint8_t hdr[PACK_HDR_SIZE];
    if (esp_partition_read(s_res, 0, hdr, sizeof(hdr)) != ESP_OK) return;
    if (memcmp(hdr, "CSRP", 4) != 0) return;
    if (u32le(hdr + 4) != 1) return;

    s_slots     = u32le(hdr + 8);
    s_alias_n   = u32le(hdr + 12);
    s_alias_off = u32le(hdr + 16);
    s_p48_off   = u32le(hdr + 20);
    s_p20_off   = u32le(hdr + 24);
    uint32_t crc    = u32le(hdr + 28);
    uint32_t total  = u32le(hdr + 32);

    if (!s_slots || s_slots > PACK_MAX_SLOTS) return;
    if (!s_alias_n || s_alias_n > PACK_MAX_ALIAS) return;
    if (total != s_res->size) return;
    if (s_alias_off < PACK_HDR_SIZE ||
        s_alias_off + s_alias_n * PACK_ALIAS_SIZE > total) return;
    if (s_p48_off < PACK_HDR_SIZE ||
        s_p48_off + s_slots * PIX48 * 2 > total) return;
    if (s_p20_off < s_p48_off + s_slots * PIX48 * 2 ||
        s_p20_off + s_slots * PIX20 * 2 > total) return;

    // CRC 覆盖 header 之后的所有字节,分块读(5KB 栈装不下 1MB)
    uint32_t crc_run = 0;
    // CRC 覆盖 header 之后的所有字节,分块读。
    // ⚠ 禁止改回栈缓冲:本函数跑在 main 任务栈上(cs_niko_start → cs_logos_init),
    // main 任务栈只有 CONFIG_ESP_MAIN_TASK_STACK_SIZE。v2.1~v2.6 用栈上 chunk[4096]
    // 直接打爆 4KB 栈 —— 症状就是开机白屏闪屏(背光亮→崩溃→重启→再白屏)。
    static uint8_t chunk[4096];
    for (uint32_t off = PACK_HDR_SIZE; off < total; off += sizeof(chunk)) {
        uint32_t n = total - off;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (esp_partition_read(s_res, off, chunk, n) != ESP_OK) return;
        crc_run = esp_crc32_le(crc_run, chunk, n);
    }
    if (crc_run != crc) {
        ESP_LOGW("cslogos", "csres CRC mismatch (%08lx != %08lx), pack disabled",
                 (unsigned long)crc_run, (unsigned long)crc);
        return;
    }

    s_alias = malloc((size_t)s_alias_n * sizeof(pack_alias_t));
    if (!s_alias) return;
    if (esp_partition_read(s_res, s_alias_off, s_alias,
                           s_alias_n * PACK_ALIAS_SIZE) != ESP_OK) {
        free(s_alias);
        s_alias = NULL;
        return;
    }

    for (int i = 0; i < POOL48_N; i++) {
        s_buf48[i] = malloc(PIX48 * 2);
        if (!s_buf48[i]) { pack_disable(); return; }
        s_pid48[i][0] = 0;
    }
    for (int i = 0; i < POOL20_N; i++) {
        s_buf20[i] = malloc(PIX20 * 2);
        if (!s_buf20[i]) { pack_disable(); return; }
        s_pid20[i][0] = 0;
    }
    ESP_LOGI("cslogos", "csres pack OK: %lu 队, %lu 条别名",
             (unsigned long)s_slots, (unsigned long)s_alias_n);
}

// 在别名表里找 slot(先精确小写比较,再归一化比较)。
static int pack_find(const char *id, const char *name)
{
    if (!s_alias) return -1;
    if (id && id[0]) {
        for (uint32_t i = 0; i < s_alias_n; i++)
            if (id_eq(s_alias[i].id, id)) return (int)s_alias[i].slot;
    }
    char nid[24];
    norm_id(name ? name : id, nid, sizeof(nid));
    if (nid[0]) {
        for (uint32_t i = 0; i < s_alias_n; i++)
            if (id_eq(s_alias[i].id, nid)) return (int)s_alias[i].slot;
    }
    return -1;
}

static void make_dsc(lv_image_dsc_t *dsc, const uint16_t *pix, uint16_t edge)
{
    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = edge;
    dsc->header.h      = edge;
    dsc->header.stride = (uint32_t)edge * 2u;
    dsc->data_size     = (uint32_t)edge * edge * 2u;
    dsc->data          = (const uint8_t *)pix;
}

// 从资源包把 slot 的像素读进池里,返回描述符(池满则覆盖最旧的一格)。
static const lv_image_dsc_t *pack_draw(int slot, bool big)
{
    if (slot < 0 || (uint32_t)slot >= s_slots) return NULL;

    if (big) {
        int i = s_pcur48++ % POOL48_N;
        uint32_t off = s_p48_off + (uint32_t)slot * PIX48 * 2;
        if (esp_partition_read(s_res, off, s_buf48[i], PIX48 * 2) != ESP_OK)
            return NULL;
        make_dsc(&s_pdsc48[i], s_buf48[i], EDGE48);
        return &s_pdsc48[i];
    }
    int i = s_pcur20++ % POOL20_N;
    uint32_t off = s_p20_off + (uint32_t)slot * PIX20 * 2;
    if (esp_partition_read(s_res, off, s_buf20[i], PIX20 * 2) != ESP_OK)
        return NULL;
    make_dsc(&s_pdsc20[i], s_buf20[i], EDGE20);
    return &s_pdsc20[i];
}

void cs_logos_init(void)
{
    if (s_ready) return;
    for (int i = 0; i < LOGO_N; i++) {
        make_dsc(&s_dsc48[i], &cs_logo48[i * PIX48], EDGE48);
        make_dsc(&s_dsc20[i], &cs_logo20[i * PIX20], EDGE20);
    }
    pack_probe();
    s_ready = true;
    if (s_alias)
        ESP_LOGI("cslogos", "%d 内嵌队标 + %lu 包队标就绪",
                 LOGO_N, (unsigned long)s_slots);
    else
        ESP_LOGI("cslogos", "%d 支战队队标就绪(无资源包)", LOGO_N);
}

// 48x48 for score cards, 20x20 for list rows. NULL if unknown;
// callers must fall back to a colour badge.
const lv_image_dsc_t *cs_logo_get(const char *id)
{
    int i = find_idx(id);
    if (i >= 0) return &s_dsc48[i];
    if (s_alias) {
        int slot = pack_find(id, NULL);
        if (slot >= 0) return pack_draw(slot, true);
    }
    return NULL;
}

const lv_image_dsc_t *cs_logo_get_small(const char *id)
{
    int i = find_idx(id);
    if (i >= 0) return &s_dsc20[i];
    if (s_alias) {
        int slot = pack_find(id, NULL);
        if (slot >= 0) return pack_draw(slot, false);
    }
    return NULL;
}

// 带队名兜底的版本:JSON 里 logo 字段为空的队伍(内嵌 105 队之外),
// 用队名归一化后到资源包里找 —— 世界前 200 的队标都从这里出。
const lv_image_dsc_t *cs_logo_get_named(const char *id, const char *name)
{
    int i = find_idx(id);
    if (i >= 0) return &s_dsc48[i];
    if (s_alias) {
        int slot = pack_find(id, name);
        if (slot >= 0) return pack_draw(slot, true);
    }
    return NULL;
}

const lv_image_dsc_t *cs_logo_get_small_named(const char *id, const char *name)
{
    int i = find_idx(id);
    if (i >= 0) return &s_dsc20[i];
    if (s_alias) {
        int slot = pack_find(id, name);
        if (slot >= 0) return pack_draw(slot, false);
    }
    return NULL;
}
