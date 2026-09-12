// cs_data.h —— 赛事数据模型 + JSON 解析 + Flash 缓存(离线优先)。
//
// 设计要点(相对旧实现):
//   * 模型与网络彻底解耦:本层不认识 Wi-Fi/HTTP,只负责"给一段 JSON,更新模型"。
//     cs_net.c(联网抓取)与 cs_portal.c(手机代理投递)都只是它的调用方。
//   * 离线优先:开机先把 Flash 缓存载入模型(立刻有画面),联网成功后再覆盖。
//   * 每一次成功更新都落盘,并递增一个版本号,UI 据此决定要不要重绘。
//
// 本层不碰 LVGL。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ---------------- 数据模型 ----------------
#define CS_MAX_MAPS     5
#define CS_MAX_MATCHES  20

#define CS_EVENT_LEN    40
#define CS_DATE_LEN     12
#define CS_TIME_LEN     8
#define CS_BO_LEN       6
#define CS_STATUS_LEN   10
#define CS_TEAM_LEN     20
#define CS_SHORT_LEN    8
// 最长的队标 id 是 "astralis-talent"(15),留 24 免得截断后查不到表。
#define CS_LOGO_LEN     24
#define CS_MAP_LEN      20
#define CS_UPDATED_LEN  24
#define CS_SOURCE_LEN   24

// 比赛状态(与 JSON 里 status 字段一致)
#define CS_ST_LIVE      "live"
#define CS_ST_FINISHED  "finished"
#define CS_ST_UPCOMING  "upcoming"

typedef struct {
    char name[CS_MAP_LEN];   // 英文图名 Mirage / Inferno(用于配色)
    char cn[CS_MAP_LEN];     // 中文图名 荒漠迷城
    int  s1;                 // 左队得分
    int  s2;                 // 右队得分
    int  winner;             // 1=左胜 2=右胜 0=未知
} cs_map_t;

typedef struct {
    char     event[CS_EVENT_LEN];
    char     stage[CS_EVENT_LEN];
    char     date[CS_DATE_LEN];
    char     time[CS_TIME_LEN];
    char     bo[CS_BO_LEN];
    char     status[CS_STATUS_LEN];
    char     t1_name[CS_TEAM_LEN];
    char     t1_short[CS_SHORT_LEN];
    char     t1_logo[CS_LOGO_LEN];
    uint32_t t1_color;
    char     t2_name[CS_TEAM_LEN];
    char     t2_short[CS_SHORT_LEN];
    char     t2_logo[CS_LOGO_LEN];
    uint32_t t2_color;
    int      score1;
    int      score2;
    int      map_count;
    cs_map_t maps[CS_MAX_MAPS];
} cs_match_t;

typedef struct {
    char       updated[CS_UPDATED_LEN];  // 数据源标注的更新时间
    char       source[CS_SOURCE_LEN];    // 本机记录的数据来源(网络/缓存/手机/内置)
    int        count;
    cs_match_t m[CS_MAX_MATCHES];
} cs_data_t;

// ---------------- 拉取状态(UI 轮询) ----------------
typedef enum {
    CS_FETCH_IDLE = 0,
    CS_FETCH_RUNNING,
    CS_FETCH_OK,
    CS_FETCH_FAIL,
} cs_fetch_state_t;

// ---------------- 读 ----------------
const cs_data_t *cs_data(void);
bool             cs_data_is_from_net(void);
const char      *cs_data_source(void);
uint32_t         cs_data_rev(void);        // 每次成功更新 +1,UI 用来判断是否需要重绘

// 按状态筛选:把 cs_data() 中 status == st 的下标写进 idx_out(最多 max 个),返回个数。
// st 传 NULL 表示不过滤。
int cs_data_filter(const char *st, int *idx_out, int max);

// ---------------- 写 ----------------
// 解析 JSON 并覆盖模型。persist=true 时同时写入 Flash 缓存(csdata 分区)。
// src 是给人看的数据来源短串(如 "网络"/"手机投递")。成功返回 true。
bool cs_data_apply_json(const char *json, bool persist, const char *src);

// 载入内置示例(首次开机、缓存损坏时的兜底,结构与线上 JSON 一致)。
void cs_data_use_builtin(void);

// 从 Flash 缓存载入;无缓存则退回内置示例。开机第一时间调用。
esp_err_t cs_data_load_persisted(void);

// 清空 Flash 缓存(调试用)。
void cs_data_clear_cache(void);

// ---------------- 状态 ----------------
cs_fetch_state_t cs_data_fetch_state(void);
const char      *cs_data_fetch_msg(void);
void             cs_data_fetch_set(cs_fetch_state_t st, const char *msg);
void             cs_data_fetch_reset(void);
