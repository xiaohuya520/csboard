// cs_logos.c -- team logo lookup, backed by compile-time embedded bitmaps.
//
// Replaces the old runtime-mmap'd csres partition: the pixel data now lives in
// cs_logo_data.c (generated) and is guaranteed byte-exact by the C compiler, so
// the "packed resource silently corrupted -> garbled glyphs" class of bug is gone.
#include "cs_logos.h"
#include "cs_fonts.h"

#include <string.h>

#include "esp_log.h"

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
    { "oddiK", 74 },
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
    { "sinnerS", 89 },
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

// BSS: descriptors only (220 x ~48B). Pixels stay in flash.
static lv_image_dsc_t s_dsc48[LOGO_N];
static lv_image_dsc_t s_dsc20[LOGO_N];
static bool          s_ready;

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

void cs_logos_init(void)
{
    if (s_ready) return;
    for (int i = 0; i < LOGO_N; i++) {
        make_dsc(&s_dsc48[i], &cs_logo48[i * PIX48], EDGE48);
        make_dsc(&s_dsc20[i], &cs_logo20[i * PIX20], EDGE20);
    }
    s_ready = true;
    ESP_LOGI("cslogos", "%d 支战队队标就绪", LOGO_N);
}

// Returns NULL when the id is unknown; callers must fall back to a colour badge.
const lv_image_dsc_t *cs_logo_get(const char *id)
{
    int i = find_idx(id);
    return i < 0 ? NULL : &s_dsc48[i];
}

const lv_image_dsc_t *cs_logo_get_small(const char *id)
{
    int i = find_idx(id);
    return i < 0 ? NULL : &s_dsc20[i];
}
