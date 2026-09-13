// cs_data.c —— 数据模型 / JSON 解析 / Flash 缓存。
#include "cs_data.h"
#include "cs_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cs_data";

#define CS_NVS_PART   "csdata"   // partitions.csv 里 cardid 之后的 64KB 数据分区
#define CS_NVS_NS     "cs"
#define CS_NVS_JSON   "json"     // 旧键(字符串),仅保留用于读取兼容
#define CS_NVS_JSON_B "jsonb"    // 现用键:blob 存整份 JSON

#define CS_JSON_MAX   16384      // 单份 JSON 上限(下载缓冲与缓存同限)

static cs_data_t        s_data;
static cs_niko_t        s_niko;
static bool             s_from_net;
static uint32_t         s_rev;
static cs_fetch_state_t s_fetch = CS_FETCH_IDLE;
static char             s_fetch_msg[64];

static void scpy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// ---------------------------------------------------------------------------
// 内置示例(断网首次开机的兜底;结构与线上 JSON 完全一致)
// ---------------------------------------------------------------------------
static const cs_data_t k_builtin = {
    .updated = "内置示例",
    .source  = "builtin",
    .count = 4,
    .m = {
        {
            .event = "ESL 职业联赛 S21", .stage = "小组赛", .date = "09-11", .time = "20:00",
            .bo = "BO3", .status = CS_ST_LIVE,
            .t1_name = "Falcons", .t1_logo = "falcons", .t1_color = 0xE43B2F,
            .t2_name = "Vitality", .t2_logo = "vitality", .t2_color = 0xFFD928,
            .score1 = 1, .score2 = 1, .map_count = 3,
            .maps = {
                { "Inferno", "炼狱小镇", 13, 9, 1 },
                { "Nuke",    "核子危机", 8, 13, 2 },
                { "Mirage",  "荒漠迷城", 0, 0, 0 },
            },
        },
        {
            .event = "BLAST 世界总决赛", .stage = "半决赛", .date = "09-10", .time = "22:30",
            .bo = "BO3", .status = CS_ST_FINISHED,
            .t1_name = "Falcons", .t1_logo = "falcons", .t1_color = 0xE43B2F,
            .t2_name = "MOUZ", .t2_logo = "mouz", .t2_color = 0xE43B2F,
            .score1 = 2, .score2 = 0, .map_count = 2,
            .maps = { { "Dust2", "炙热沙城", 13, 7, 1 }, { "Ancient", "远古遗迹", 13, 10, 1 } },
        },
        {
            .event = "IEM 卡托维兹", .stage = "四分之一决赛", .date = "09-09", .time = "19:00",
            .bo = "BO3", .status = CS_ST_FINISHED,
            .t1_name = "Falcons", .t1_logo = "falcons", .t1_color = 0xE43B2F,
            .t2_name = "FaZe", .t2_logo = "faze", .t2_color = 0xB01C24,
            .score1 = 1, .score2 = 2, .map_count = 3,
            .maps = {
                { "Mirage", "荒漠迷城", 13, 11, 1 },
                { "Overpass", "死亡游乐园", 9, 13, 2 },
                { "Nuke", "核子危机", 11, 13, 2 },
            },
        },
        {
            .event = "ESL 职业联赛 S21", .stage = "小组赛", .date = "09-12", .time = "18:00",
            .bo = "BO3", .status = CS_ST_UPCOMING,
            .t1_name = "Falcons", .t1_logo = "falcons", .t1_color = 0xE43B2F,
            .t2_name = "G2", .t2_logo = "g2", .t2_color = 0xE4AE39,
            .score1 = 0, .score2 = 0, .map_count = 0,
        },
    },
};

// NiKo 内置示例(断网首次开机的兜底;与线上 JSON 的 niko 字段一致)
static const cs_niko_t k_niko_builtin = {
    .name      = "NiKo",
    .realname  = "Nikola Kovač",
    .team      = "Falcons",
    .team_logo = "falcons",
    .team_color= 0xE43B2F,
    .role      = "步枪手 / 指挥",
    .rating    = 1.12f,
    .kd        = 1.08f,
    .adr       = 78.5f,
    .kast      = 74.3f,
    .impact    = 1.05f,
    .maps      = 1480,
    .majors    = 0,
    .mvp       = 21,
    .earnings  = 190,
    .years     = "2009-至今",
    .age       = 28,
};

// ---------------------------------------------------------------------------
// NVS:优先用 csdata 资源分区,不可用则退回默认 nvs
// ---------------------------------------------------------------------------
static int s_part = -1;   // -1 未探测,1 可用,0 不可用

static bool part_ready(void)
{
    if (s_part >= 0) return s_part == 1;
    if (cs_sys_nvs() != ESP_OK) { s_part = 0; return false; }
    esp_err_t err = nvs_flash_init_partition(CS_NVS_PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "%s 需重建", CS_NVS_PART);
        nvs_flash_erase_partition(CS_NVS_PART);
        err = nvs_flash_init_partition(CS_NVS_PART);
    }
    s_part = (err == ESP_OK) ? 1 : 0;
    if (!s_part) ESP_LOGW(TAG, "分区 %s 不可用(%s),缓存退回默认 nvs",
                          CS_NVS_PART, esp_err_to_name(err));
    return s_part == 1;
}

static esp_err_t kv_open(nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (part_ready() &&
        nvs_open_from_partition(CS_NVS_PART, CS_NVS_NS, mode, out) == ESP_OK) {
        return ESP_OK;
    }
    return nvs_open(CS_NVS_NS, mode, out);
}

// ---------------------------------------------------------------------------
// JSON 解析
// ---------------------------------------------------------------------------
static void jstr(cJSON *o, const char *key, char *dst, size_t cap, const char *def)
{
    cJSON *j = o ? cJSON_GetObjectItem(o, key) : NULL;
    scpy(dst, cap, (j && cJSON_IsString(j)) ? j->valuestring : def);
}

static uint32_t jcolor(cJSON *o, const char *key, uint32_t def)
{
    cJSON *j = o ? cJSON_GetObjectItem(o, key) : NULL;
    if (!j || !cJSON_IsString(j)) return def;
    const char *s = j->valuestring;
    if (*s == '#') s++;
    unsigned v = (unsigned)strtoul(s, NULL, 16);
    return v ? (v & 0xFFFFFFu) : def;
}

static int jint(cJSON *o, const char *key, int def)
{
    cJSON *j = o ? cJSON_GetObjectItem(o, key) : NULL;
    return cJSON_IsNumber(j) ? j->valueint : def;
}

static float jfloat(cJSON *o, const char *key, float def)
{
    cJSON *j = o ? cJSON_GetObjectItem(o, key) : NULL;
    return (j && cJSON_IsNumber(j)) ? (float)j->valuedouble : def;
}

// 解析到调用方提供的结构体(必须是堆上的:cs_data_t 约 10KB,绝不能放任务栈)。
static bool parse_into(const char *json, cs_data_t *out, cs_niko_t *nk_out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON 解析失败(cJSON)");
        return false;
    }

    memset(out, 0, sizeof(*out));
    jstr(root, "updated", out->updated, sizeof(out->updated), "net");

    cJSON *arr = cJSON_GetObjectItem(root, "matches");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "缺少 matches 数组");
        cJSON_Delete(root);
        return false;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > CS_MAX_MATCHES) n = CS_MAX_MATCHES;

    for (int i = 0; i < n; i++) {
        cJSON *mi = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(mi)) continue;

        cs_match_t *m = &out->m[out->count];
        jstr(mi, "event",  m->event,  sizeof(m->event),  "CS2 赛事");
        jstr(mi, "stage",  m->stage,  sizeof(m->stage),  "");
        jstr(mi, "date",   m->date,   sizeof(m->date),   "");
        jstr(mi, "time",   m->time,   sizeof(m->time),   "");
        jstr(mi, "bo",     m->bo,     sizeof(m->bo),     "BO3");
        jstr(mi, "status", m->status, sizeof(m->status), CS_ST_UPCOMING);

        cJSON *t1 = cJSON_GetObjectItem(mi, "team1");
        jstr(t1, "name",  m->t1_name,  sizeof(m->t1_name),  "TBD");
        jstr(t1, "short", m->t1_short, sizeof(m->t1_short), "");
        jstr(t1, "logo",  m->t1_logo,  sizeof(m->t1_logo),  "");
        m->t1_color = jcolor(t1, "color", 0xE43B2F);

        cJSON *t2 = cJSON_GetObjectItem(mi, "team2");
        jstr(t2, "name",  m->t2_name,  sizeof(m->t2_name),  "TBD");
        jstr(t2, "short", m->t2_short, sizeof(m->t2_short), "");
        jstr(t2, "logo",  m->t2_logo,  sizeof(m->t2_logo),  "");
        m->t2_color = jcolor(t2, "color", 0x2AA3EF);

        m->score1 = jint(mi, "score1", 0);
        m->score2 = jint(mi, "score2", 0);

        cJSON *maps = cJSON_GetObjectItem(mi, "maps");
        if (cJSON_IsArray(maps)) {
            int mn = cJSON_GetArraySize(maps);
            if (mn > CS_MAX_MAPS) mn = CS_MAX_MAPS;
            for (int k = 0; k < mn; k++) {
                cJSON *km = cJSON_GetArrayItem(maps, k);
                if (!cJSON_IsObject(km)) continue;
                cs_map_t *mp = &m->maps[m->map_count];
                jstr(km, "name", mp->name, sizeof(mp->name), "Map");
                jstr(km, "cn",   mp->cn,   sizeof(mp->cn),   "");
                mp->s1 = jint(km, "s1", 0);
                mp->s2 = jint(km, "s2", 0);
                mp->winner = jint(km, "winner", mp->s1 > mp->s2 ? 1 : (mp->s2 > mp->s1 ? 2 : 0));
                m->map_count++;
            }
        }
        out->count++;
    }

    // NiKo 衍生字段(缺失则保留内置示例,避免空指针)
    cJSON *nk = cJSON_GetObjectItem(root, "niko");
    if (nk && cJSON_IsObject(nk) && nk_out) {
        jstr(nk, "name",      nk_out->name,      sizeof(nk_out->name),      "NiKo");
        jstr(nk, "realname",  nk_out->realname,  sizeof(nk_out->realname),  "");
        jstr(nk, "team",      nk_out->team,      sizeof(nk_out->team),      "");
        jstr(nk, "logo",      nk_out->team_logo, sizeof(nk_out->team_logo), "");
        nk_out->team_color = jcolor(nk, "color", 0xE43B2F);
        jstr(nk, "role",      nk_out->role,      sizeof(nk_out->role),      "");
        nk_out->rating   = jfloat(nk, "rating", 0);
        nk_out->kd       = jfloat(nk, "kd", 0);
        nk_out->adr      = jfloat(nk, "adr", 0);
        nk_out->kast     = jfloat(nk, "kast", 0);
        nk_out->impact   = jfloat(nk, "impact", 0);
        nk_out->maps     = jint(nk, "maps", 0);
        nk_out->majors   = jint(nk, "majors", 0);
        nk_out->mvp      = jint(nk, "mvp", 0);
        nk_out->earnings = jint(nk, "earnings", 0);
        jstr(nk, "years", nk_out->years, sizeof(nk_out->years), "");
        nk_out->age      = jint(nk, "age", 0);
    }

    cJSON_Delete(root);
    return out->count > 0;
}

// ---------------------------------------------------------------------------
// 缓存
// ---------------------------------------------------------------------------
static esp_err_t cache_save(const char *json)
{
    nvs_handle_t h;
    esp_err_t err = kv_open(NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // 必须用 blob:NVS 单条字符串上限 4000 字节,而赛程 JSON 已 6KB+,
    // nvs_set_str 会直接返回 ESP_ERR_NVS_VALUE_TOO_LONG —— 缓存永远写不进去,
    // 表现为"每次冷启动都退回内置示例"。blob 只受分区容量限制(csdata 有 64KB)。
    // 连结尾的 NUL 一起存,读出来就是现成的 C 字符串。
    err = nvs_set_blob(h, CS_NVS_JSON_B, json, strlen(json) + 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t cache_load(char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t err = kv_open(NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = cap;
    err = nvs_get_blob(h, CS_NVS_JSON_B, out, &len);
    if (err == ESP_OK) {
        out[(len > 0 && len < cap) ? len - 1 : cap - 1] = '\0';
    } else if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH) {
        len = cap;                       // 兼容旧固件留下的字符串键
        err = nvs_get_str(h, CS_NVS_JSON, out, &len);
    }
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// 对外
// ---------------------------------------------------------------------------
const cs_data_t *cs_data(void)        { return &s_data; }
bool             cs_data_is_from_net(void) { return s_from_net; }
const char      *cs_data_source(void) { return s_data.source; }
uint32_t         cs_data_rev(void)    { return s_rev; }

const cs_niko_t *cs_niko(void)         { return &s_niko; }
const char      *cs_follow_team(void)  { return s_niko.team; }
void             cs_niko_use_builtin(void) { s_niko = k_niko_builtin; }

int cs_data_filter(const char *st, int *idx_out, int max)
{
    int n = 0;
    if (!idx_out || max <= 0) return 0;
    for (int i = 0; i < s_data.count && n < max; i++) {
        if (!st || strcmp(s_data.m[i].status, st) == 0) idx_out[n++] = i;
    }
    return n;
}

void cs_data_use_builtin(void)
{
    s_data = k_builtin;
    s_niko = k_niko_builtin;
    s_from_net = false;
    s_rev++;
    scpy(s_fetch_msg, sizeof(s_fetch_msg), "内置示例数据");
}

bool cs_data_apply_json(const char *json, bool persist, const char *src)
{
    if (!json || !json[0]) return false;

    // 堆上解析:cs_data_t 约 10KB,放任务栈就是 v4.3 那次白屏重启的翻版。
    cs_data_t *tmp = (cs_data_t *)malloc(sizeof(cs_data_t));
    if (!tmp) {
        ESP_LOGE(TAG, "解析缓冲分配失败");
        return false;
    }
    bool ok = parse_into(json, tmp, &s_niko);
    if (!ok) {
        free(tmp);
        return false;
    }

    scpy(tmp->source, sizeof(tmp->source), src ? src : "net");
    s_data = *tmp;
    free(tmp);

    s_from_net = true;
    s_rev++;

    if (persist) {
        esp_err_t err = cache_save(json);
        if (err != ESP_OK) ESP_LOGW(TAG, "缓存写入失败: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "数据已更新: %d 场(来源 %s)", s_data.count, s_data.source);
    return true;
}

esp_err_t cs_data_load_persisted(void)
{
    char *buf = (char *)malloc(CS_JSON_MAX);
    if (!buf) { cs_data_use_builtin(); return ESP_ERR_NO_MEM; }

    esp_err_t err = cache_load(buf, CS_JSON_MAX);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGW(TAG, "无缓存(%s),用内置示例", esp_err_to_name(err));
        cs_data_use_builtin();
        return err;
    }
    cs_data_t *tmp = (cs_data_t *)malloc(sizeof(cs_data_t));
    if (!tmp) { free(buf); cs_data_use_builtin(); return ESP_ERR_NO_MEM; }

    bool ok = parse_into(buf, tmp, &s_niko);
    free(buf);
    if (!ok) {
        free(tmp);
        ESP_LOGW(TAG, "缓存解析失败,用内置示例");
        cs_data_use_builtin();
        return ESP_ERR_INVALID_RESPONSE;
    }
    scpy(tmp->source, sizeof(tmp->source), "cache");
    s_data = *tmp;
    free(tmp);
    s_from_net = false;
    s_rev++;
    scpy(s_fetch_msg, sizeof(s_fetch_msg), "已载入本地缓存");
    ESP_LOGI(TAG, "已载入缓存: %d 场", s_data.count);
    return ESP_OK;
}

void cs_data_clear_cache(void)
{
    nvs_handle_t h;
    if (kv_open(NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, CS_NVS_JSON);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "缓存已清除");
}

cs_fetch_state_t cs_data_fetch_state(void) { return s_fetch; }
const char      *cs_data_fetch_msg(void)   { return s_fetch_msg; }
void cs_data_fetch_reset(void) { if (s_fetch != CS_FETCH_RUNNING) s_fetch = CS_FETCH_IDLE; }

void cs_data_fetch_set(cs_fetch_state_t st, const char *msg)
{
    s_fetch = st;
    if (msg) scpy(s_fetch_msg, sizeof(s_fetch_msg), msg);
}
