// cs_net.h —— Wi-Fi 管理 + 赛事数据联网抓取。
//
// 相对旧实现的根本改动(针对"网络无法更新赛事"):
//   1) 多镜像回退:同一份 JSON 在 jsDelivr 多个节点 + GitHub raw + 多个代理上各放一份,
//      逐个尝试,任一成功即止。单个域名被墙/抽风不再等于"更新不了"。
//   2) HTTPS 前置校时:上电 RTC 是 1970 年,mbedTLS 校验证书有效期必然失败,表现正是
//      ESP_ERR_HTTP_CONNECT。抓取前先等 SNTP 同步(最多 15s),从根上掐掉这一类假故障。
//   3) DNS 兜底:DHCP 下发的 DNS 拿不到或解析失败时,改用 223.5.5.5 / 119.29.29.29。
//   4) 故障分类:把失败归到 DNS / 连接 / 证书 / HTTP / 空数据 / 解析,并给出中文短句上屏,
//      用户拍照即可定位是"网的问题"还是"板子的问题"。
//   5) 指数退避:失败从 30s 翻倍到最多 5min,成功则 5min 一轮,不再高频刷屏。
//   6) 配网页零网络路径:见 cs_portal.c —— 手机浏览器代抓后投递给板子。
//
// 本层不碰 LVGL。耗时动作都在后台任务里,UI 通过 cs_net_tick() 轮询状态。
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CS_NET_OFF = 0,      // 未初始化
    CS_NET_READY,        // 已初始化,空闲
    CS_NET_SCANNING,     // 扫描中
    CS_NET_APLIST,       // 扫描完成,列表可读
    CS_NET_CONNECTING,   // 正在连接
    CS_NET_ONLINE,       // 已拿到 IP
    CS_NET_FAILED,       // 扫描/连接失败
} cs_net_state_t;

// 故障分类:用于把"为什么更新不了"讲到点上
typedef enum {
    CS_DIAG_NONE = 0,
    CS_DIAG_NO_WIFI,     // 未连上 Wi-Fi
    CS_DIAG_TIME,        // 设备时间未同步(TLS 证书校验会失败)
    CS_DIAG_DNS,         // 域名解析失败
    CS_DIAG_TCP,         // 连不上服务器
    CS_DIAG_TLS,         // TLS/证书失败
    CS_DIAG_HTTP,        // 服务器返回非 200
    CS_DIAG_EMPTY,       // 收到的数据为空
    CS_DIAG_PARSE,       // 数据格式不对
    CS_DIAG_MEM,         // 内存不足
} cs_diag_t;

// 初始化 Wi-Fi(幂等)。成功后用已保存凭证自动重连,没有凭证则开始扫描。
esp_err_t cs_net_init(void);

cs_net_state_t cs_net_state(void);
void           cs_net_state_reset(void);   // FAILED -> READY
bool           cs_net_online(void);
bool           cs_net_time_synced(void);   // 设备时钟是否已可信(>2023)
const char    *cs_net_ip(void);
const char    *cs_net_ssid(void);
const char    *cs_net_conn_err(void);      // 最近一次连接失败的中文短句

// 扫描(异步)。完成后 state 变 CS_NET_APLIST 或 CS_NET_FAILED。
void cs_net_scan(void);
bool cs_net_scan_busy(void);
const char *cs_net_scan_msg(void);

int         cs_net_ap_count(void);
const char *cs_net_ap_ssid(int idx);
int         cs_net_ap_rssi(int idx);       // dBm
bool        cs_net_ap_secure(int idx);
const char *cs_net_ap_prev_ssid(void);     // 上次连过的 SSID(列表里打标)

void cs_net_connect(const char *ssid, const char *password);
void cs_net_autoconnect(void);
void cs_net_forget(void);

// 周期维护。UI 定时器每 200ms 调一次:扫描/连接看门狗、断线自动重连、
// 失败退避重试、按需自动拉取都靠它驱动。
void cs_net_tick(void);

// ---------------- 数据源 ----------------
// 用户自定义 URL(存 NVS);为空时走内置镜像列表。
const char *cs_net_custom_url(void);
void        cs_net_set_custom_url(const char *url);
const char *cs_net_url_now(void);          // 当前优先尝试的 URL(展示用)
const char *cs_net_mirror_name(void);      // 最近一次成功的镜像名(空=无)

// ---------------- 拉取 ----------------
// 异步触发一次拉取。force=true 立即开始并重置退避。
void cs_net_refresh(bool force);

// ---------------- 诊断 ----------------
cs_diag_t   cs_net_diag(void);
const char *cs_net_diag_text(void);        // 可直接上屏的中文短句

// 北京时间 HH:MM;未同步写 "--:--"。
void cs_time_hhmm(char *out, int n);
