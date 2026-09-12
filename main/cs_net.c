// cs_net.c —— Wi-Fi 管理 + 多镜像抓取 + 诊断 + 退避。
#include "cs_net.h"
#include "cs_data.h"
#include "cs_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"

static const char *TAG = "cs_net";

// ---------------------------------------------------------------------------
// 数据源:同一份 JSON 的多个镜像。逐个尝试,任一成功即止。
//   * 前几个走 jsDelivr 的多个节点(国内普遍可达);
//   * GitHub raw 没有 CDN 缓存,数据最"新鲜",但部分网络解析不了;
//   * Statically 作为另一条独立线路。
// 数据由 .github/workflows/update-matches.yml 定时从仓库 main 分支更新。
// ---------------------------------------------------------------------------
static const struct {
    const char *url;
    const char *name;
} MIRRORS[] = {
    { "https://cdn.jsdelivr.net/gh/xiaohuya520/csboard@main/cs_matches.json",        "jsDelivr"      },
    { "https://fastly.jsdelivr.net/gh/xiaohuya520/csboard@main/cs_matches.json",     "jsDelivr-East" },
    { "https://gcore.jsdelivr.net/gh/xiaohuya520/csboard@main/cs_matches.json",      "jsDelivr-Gcore"},
    { "https://testingcf.jsdelivr.net/gh/xiaohuya520/csboard@main/cs_matches.json",  "jsDelivr-CF"   },
    { "https://raw.githubusercontent.com/xiaohuya520/csboard/main/cs_matches.json",  "GitHub-Raw"    },
    { "https://cdn.statically.io/gh/xiaohuya520/csboard/main/cs_matches.json",       "Statically"    },
};
#define MIRROR_N (sizeof(MIRRORS) / sizeof(MIRRORS[0]))

#define CS_NVS_NS        "net"
#define CS_NVS_URL_KEY   "url"
#define CS_JSON_MAX      16384     // 单份 JSON 上限
#define CS_AP_MAX        16
#define CS_TICK_MS       200
#define CS_TIME_MIN      1700000000LL   // 2023-11;低于它说明时钟不可信

#define CS_SCAN_MIN_MS   80
#define CS_SCAN_MAX_MS   220
#define CS_BACKOFF_MIN   30        // 失败退避起点(秒)
#define CS_BACKOFF_MAX   300       // 退避上限,同时也是成功后的轮询间隔
#define CS_RETRY_TICKS   4         // 断线重连节拍基数:4 x 200ms = 0.8s,按次数递增

// ---------------------------------------------------------------------------
// 状态
// ---------------------------------------------------------------------------
static cs_net_state_t   s_state = CS_NET_OFF;
static bool             s_wifi_ready;
static bool             s_wifi_started;
static bool             s_sntp_started;
static esp_netif_t     *s_sta;
static esp_event_handler_instance_t s_h_wifi;
static esp_event_handler_instance_t s_h_ip;
static char             s_ip[20];
static char             s_ssid[33];
static char             s_prev_ssid[33];
static int              s_reconnect;

static wifi_ap_record_t s_aps[CS_AP_MAX];
static uint16_t         s_ap_count;
static volatile bool    s_scan_busy;
static bool             s_scan_keep_online;
static int              s_scan_watch;
static char             s_scan_msg[48];

static char             s_conn_err[48];
static int              s_conn_watch;
static int              s_retry_watch;      // 断线自动重连的节拍
static int              s_retry_pending = -1;  // >0 时倒数几拍后重连;断线回调只登记不硬连

static cs_diag_t        s_diag;
static char             s_custom_url[192];

static volatile bool    s_fetching;
static char             s_mirror_name[20];
static int              s_due;              // 距下次自动拉取的 tick 数(200ms/tick)
static int              s_backoff_s = CS_BACKOFF_MIN;

static void scpy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void diag_set(cs_diag_t d) { s_diag = d; }

// ---------------------------------------------------------------------------
// SNTP:时间不可信时 HTTPS 一定失败,所以必须先校时
// ---------------------------------------------------------------------------
bool cs_net_time_synced(void)
{
    return (long long)time(NULL) > CS_TIME_MIN;
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.start = true;
#if CONFIG_LWIP_SNTP_MAX_SERVERS > 1
    cfg.servers[1] = "cn.pool.ntp.org";      // 首选不通时还有备用池
    cfg.num_of_servers = 2;
#endif
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP 已启动");
    } else {
        ESP_LOGW(TAG, "SNTP 启动失败");
    }
}

// 阻塞等待时钟生效(毫秒上限)。返回是否成功。
static bool wait_clock(int max_ms)
{
    if (cs_net_time_synced()) return true;
    start_sntp();
    for (int waited = 0; waited < max_ms; waited += 250) {
        if (cs_net_time_synced()) {
            ESP_LOGI(TAG, "校时完成");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(TAG, "等待校时超时");
    return false;
}

// ---------------------------------------------------------------------------
// DNS 兜底:DHCP 没给 DNS 或给的不通时,换成公共 DNS
// ---------------------------------------------------------------------------
static void dns_set(esp_netif_dns_type_t type, const char *ip)
{
    if (!s_sta) return;
    esp_netif_dns_info_t d;
    memset(&d, 0, sizeof(d));
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = ipaddr_addr(ip);
    esp_err_t err = esp_netif_set_dns_info(s_sta, type, &d);
    if (err != ESP_OK) ESP_LOGW(TAG, "设置 DNS %s 失败: %s", ip, esp_err_to_name(err));
}

static void ensure_dns(void)
{
    esp_netif_dns_info_t cur;
    memset(&cur, 0, sizeof(cur));
    bool have = (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &cur) == ESP_OK) &&
                cur.ip.u_addr.ip4.addr != 0;
    if (!have) {
        ESP_LOGW(TAG, "DHCP 未下发有效 DNS,改用 223.5.5.5");
        dns_set(ESP_NETIF_DNS_MAIN, "223.5.5.5");
    }
    // 备用 DNS 总是补上:主 DNS 抽风时还有一条路。
    dns_set(ESP_NETIF_DNS_BACKUP, "119.29.29.29");
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
// 把 reason 翻成一句人话。对照 ESP-IDF wifi_err_reason_t:
//   200 信标超时 / 201 扫不到 / 202 认证失败 / 203-205 关联被路由器拒 /
//   206-207 路由器侧漫游 / 208-209 AP 没回 SA query /
//   210-212 扫到了同名 AP,但加密方式或信号门槛不满足。
// 210 保留数字:它最常见的成因是路由器开了 WPA3/PMF,或存在同名开放热点。
static void conn_err_set(int reason)
{
    switch (reason) {
    case 2:   scpy(s_conn_err, sizeof(s_conn_err), "认证超时,信号弱");   break;
    case 15:  scpy(s_conn_err, sizeof(s_conn_err), "密码可能不对");       break;
    case 200: scpy(s_conn_err, sizeof(s_conn_err), "信号不稳,靠近些");    break;
    case 201: scpy(s_conn_err, sizeof(s_conn_err), "扫不到该网络");       break;
    case 202: scpy(s_conn_err, sizeof(s_conn_err), "密码可能不对");       break;
    case 203:
    case 204:
    case 205: scpy(s_conn_err, sizeof(s_conn_err), "连接被路由器拒绝");   break;
    case 206:
    case 207: scpy(s_conn_err, sizeof(s_conn_err), "路由器切换中");       break;
    case 208:
    case 209: scpy(s_conn_err, sizeof(s_conn_err), "路由器响应超时");     break;
    default:
        if (reason >= 210 && reason <= 212)
            snprintf(s_conn_err, sizeof(s_conn_err), "加密不兼容(%d)", reason);
        else
            snprintf(s_conn_err, sizeof(s_conn_err), "连接失败 代码%d", reason);
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != WIFI_EVENT_STA_DISCONNECTED) return;

    const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
    int reason = d ? (int)d->reason : -1;
    ESP_LOGW(TAG, "Wi-Fi 断开, reason=%d rssi=%d", reason, d ? (int)d->rssi : 0);
    s_ip[0] = 0;
    if (s_scan_busy) return;                  // 扫描期间不自动重连,否则会打断扫描
    if (s_state != CS_NET_CONNECTING && s_state != CS_NET_ONLINE) return;

    conn_err_set(reason);

    // 不在这里直接 esp_wifi_connect():本回调跑在系统事件任务上,同步重连会把它压住;
    // 而且刚断开就抢连,在 201/210 这类本轮没扫到的场景下毫无意义。
    // 只登记一个递增延迟,由 cs_net_tick() 去连。
    s_state = CS_NET_CONNECTING;
    s_conn_watch = 0;
    if (s_reconnect < 4) {
        s_reconnect++;
        s_retry_pending = CS_RETRY_TICKS * s_reconnect;   // 0.8 / 1.6 / 2.4 / 3.2 秒
    } else {
        s_retry_pending = -1;
        s_state = CS_NET_FAILED;                          // 转入 30s 慢速重连
        s_retry_watch = 0;
        ESP_LOGW(TAG, "连续重连失败,转入慢速重试");
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_state = CS_NET_ONLINE;
    s_reconnect = 0;
    s_conn_err[0] = 0;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) scpy(s_ssid, sizeof(s_ssid), (const char *)ap.ssid);
    ESP_LOGI(TAG, "已联网: %s IP=%s 堆余量=%u", s_ssid, s_ip,
             (unsigned)esp_get_free_heap_size());

    ensure_dns();
    start_sntp();
    s_due = 5;                                 // 联网后 1s 内安排一次自动拉取
}

static esp_err_t wifi_bring_up(void)
{
    if (s_wifi_started) return ESP_OK;

    esp_err_t err = cs_sys_nvs();
    if (err != ESP_OK) return err;
    err = cs_sys_netif();
    if (err != ESP_OK) return err;

    if (!s_sta) {
        s_sta = esp_netif_create_default_wifi_sta();
        if (!s_sta) return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_ready) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        s_wifi_ready = true;
    }

    if (!s_h_wifi) {
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &s_h_wifi);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, &s_h_ip);
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_storage: %s", esp_err_to_name(err));
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_mode: %s", esp_err_to_name(err));
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_wifi_started = true;

    // 关省电:省电模式下部分路由器会让 TCP 卡在半路,表现为"能连上但拉不到数据"。
    esp_wifi_set_ps(WIFI_PS_NONE);

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK)
        scpy(s_prev_ssid, sizeof(s_prev_ssid), (const char *)wc.sta.ssid);

    return ESP_OK;
}

esp_err_t cs_net_init(void)
{
    if (s_state != CS_NET_OFF) return ESP_OK;

    esp_err_t err = wifi_bring_up();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        s_state = CS_NET_FAILED;
        scpy(s_conn_err, sizeof(s_conn_err), "Wi-Fi 启动失败");
        return err;
    }
    s_state = CS_NET_READY;
    s_diag = CS_DIAG_NONE;

    if (s_prev_ssid[0]) cs_net_autoconnect();
    else                cs_net_scan();
    return ESP_OK;
}

cs_net_state_t cs_net_state(void)        { return s_state; }
bool           cs_net_online(void)       { return s_state == CS_NET_ONLINE; }
const char    *cs_net_ip(void)           { return s_ip; }
const char    *cs_net_ssid(void)         { return s_ssid; }
const char    *cs_net_conn_err(void)     { return s_conn_err; }
const char    *cs_net_ap_prev_ssid(void) { return s_prev_ssid; }

void cs_net_state_reset(void)
{
    if (s_state == CS_NET_FAILED) s_state = CS_NET_READY;
    s_scan_msg[0] = 0;
    s_conn_err[0] = 0;
    s_conn_watch = 0;
    s_reconnect = 0;
    s_retry_pending = -1;
}

// ---------------------------------------------------------------------------
// 扫描(阻塞式,独立任务 —— 事件式扫描一旦丢事件就永远停在"扫描中")
// ---------------------------------------------------------------------------
static void scan_task(void *arg)
{
    (void)arg;

    if (s_state == CS_NET_CONNECTING) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    wifi_scan_config_t cfg = { 0 };
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.show_hidden = false;
    cfg.scan_time.active.min = CS_SCAN_MIN_MS;
    cfg.scan_time.active.max = CS_SCAN_MAX_MS;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    uint16_t total = 0, got = 0;

    if (err == ESP_OK) err = esp_wifi_scan_get_ap_num(&total);
    if (err == ESP_OK) {
        uint16_t cap = CS_AP_MAX;             // 先看总数再取,buffer 小于总数会报错
        esp_err_t g = esp_wifi_scan_get_ap_records(&cap, s_aps);
        if (g != ESP_OK) err = g;
        else {
            got = (cap > CS_AP_MAX) ? CS_AP_MAX : cap;
            if (got > total) got = total;
        }
    }

    if (err == ESP_OK) {
        s_ap_count = got;
        s_state = s_scan_keep_online ? CS_NET_ONLINE : CS_NET_APLIST;
        s_scan_msg[0] = 0;
        ESP_LOGI(TAG, "扫描完成: 取到 %u 个(共 %u 个)", (unsigned)got, (unsigned)total);
    } else {
        s_ap_count = 0;
        s_state = s_scan_keep_online ? CS_NET_ONLINE : CS_NET_FAILED;
        scpy(s_scan_msg, sizeof(s_scan_msg), "扫描失败,按确定重试");
        ESP_LOGE(TAG, "扫描失败: %s", esp_err_to_name(err));
    }

    s_scan_busy = false;
    vTaskDelete(NULL);
}

void cs_net_scan(void)
{
    if (s_scan_busy) return;
    if (!s_wifi_started && wifi_bring_up() != ESP_OK) {
        s_state = CS_NET_FAILED;
        scpy(s_scan_msg, sizeof(s_scan_msg), "Wi-Fi 启动失败");
        return;
    }
    s_scan_keep_online = (s_state == CS_NET_ONLINE);
    s_scan_busy = true;
    s_scan_watch = 0;
    s_ap_count = 0;
    s_scan_msg[0] = 0;
    s_state = CS_NET_SCANNING;
    if (xTaskCreate(scan_task, "cs_scan", 4096, NULL, 6, NULL) != pdPASS) {
        s_scan_busy = false;
        s_state = CS_NET_FAILED;
        scpy(s_scan_msg, sizeof(s_scan_msg), "扫描任务创建失败");
    }
}

bool        cs_net_scan_busy(void) { return s_scan_busy; }
const char *cs_net_scan_msg(void)  { return s_scan_msg; }

int         cs_net_ap_count(void)     { return s_ap_count; }
const char *cs_net_ap_ssid(int idx)   { return (idx >= 0 && idx < (int)s_ap_count) ? (const char *)s_aps[idx].ssid : ""; }
int         cs_net_ap_rssi(int idx)   { return (idx >= 0 && idx < (int)s_ap_count) ? s_aps[idx].rssi : -99; }
bool        cs_net_ap_secure(int idx) { return (idx >= 0 && idx < (int)s_ap_count) && s_aps[idx].authmode != WIFI_AUTH_OPEN; }

void cs_net_connect(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) return;
    if (wifi_bring_up() != ESP_OK) { s_state = CS_NET_FAILED; return; }

    s_conn_err[0] = 0;
    s_conn_watch = 0;
    s_reconnect = 0;
    s_retry_pending = -1;

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    scpy((char *)wc.sta.ssid, sizeof(wc.sta.ssid), ssid);
    if (password) scpy((char *)wc.sta.password, sizeof(wc.sta.password), password);
    // authmode 不做门槛过滤:门槛一旦把 AP 筛掉,报出来的 reason 和"没这个网"长得一样,
    // 极难排查。密码对不对交给握手阶段判,原因更准。
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    // 全信道扫描。FAST_SCAN 沿用上次连接的信道,路由器一换信道就永远找不到 AP ——
    // 这正是"第一次能连、过一会断了就再也连不上"最常见的成因。
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // WPA3 混合模式的路由器少了 PMF capable 会握手失败(表现为"密码明明对却连不上")。
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        s_state = CS_NET_FAILED;
        scpy(s_conn_err, sizeof(s_conn_err), "网络配置失败");
        return;
    }
    scpy(s_ssid, sizeof(s_ssid), ssid);
    scpy(s_prev_ssid, sizeof(s_prev_ssid), ssid);
    s_state = CS_NET_CONNECTING;
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "正在连接 %s", ssid);
}

void cs_net_autoconnect(void)
{
    if (!s_wifi_started) return;
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK || !wc.sta.ssid[0]) return;

    // 每次都重新下发这几项。NVS 里存的可能是上一版固件写下的 FAST_SCAN + 高 authmode 门槛,
    // 那正是"断一次以后再也连不上"的元凶,不能沿用。
    wc.sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method        = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable    = true;
    wc.sta.pmf_cfg.required   = false;
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    scpy(s_ssid, sizeof(s_ssid), (const char *)wc.sta.ssid);
    s_reconnect = 0;
    s_conn_err[0] = 0;
    s_conn_watch = 0;
    s_retry_pending = -1;
    s_state = CS_NET_CONNECTING;
    esp_wifi_disconnect();      // 先收干净再连,避免上一次的残留状态参与
    esp_wifi_connect();
    ESP_LOGI(TAG, "用已保存凭证连接 %s", s_ssid);
}

void cs_net_forget(void)
{
    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_prev_ssid[0] = 0;
    s_ssid[0] = 0;
    s_ip[0] = 0;
    s_retry_pending = -1;
    if (s_wifi_started) esp_wifi_disconnect();
    s_state = CS_NET_READY;
}

// ---------------------------------------------------------------------------
// 数据源 URL
// ---------------------------------------------------------------------------
const char *cs_net_custom_url(void)
{
    if (s_custom_url[0]) return s_custom_url;
    nvs_handle_t h;
    if (cs_sys_nvs() == ESP_OK && nvs_open(CS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_custom_url);
        if (nvs_get_str(h, CS_NVS_URL_KEY, s_custom_url, &len) != ESP_OK) s_custom_url[0] = 0;
        nvs_close(h);
    }
    return s_custom_url;
}

void cs_net_set_custom_url(const char *url)
{
    scpy(s_custom_url, sizeof(s_custom_url), url);
    nvs_handle_t h;
    if (cs_sys_nvs() == ESP_OK && nvs_open(CS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (s_custom_url[0]) nvs_set_str(h, CS_NVS_URL_KEY, s_custom_url);
        else                 nvs_erase_key(h, CS_NVS_URL_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "自定义数据源: %s", s_custom_url[0] ? s_custom_url : "(已清除)");
}

const char *cs_net_url_now(void)
{
    const char *u = cs_net_custom_url();
    return u[0] ? u : MIRRORS[0].url;
}

const char *cs_net_mirror_name(void) { return s_mirror_name; }

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
typedef struct {
    char *buf;
    int   len;
    int   cap;
} http_sink_t;

static esp_err_t http_on_data(esp_http_client_event_t *e)
{
    http_sink_t *s = (http_sink_t *)e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !s || e->data_len <= 0) return ESP_OK;
    int room = s->cap - s->len - 1;
    if (room > 0) {
        int cp = e->data_len < room ? e->data_len : room;
        memcpy(s->buf + s->len, e->data, (size_t)cp);
        s->len += cp;
        s->buf[s->len] = 0;
    }
    return ESP_OK;
}

// 取 URL 的主机名(用于 DNS 探测)。
static bool url_host(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *end = p;
    while (*end && *end != '/' && *end != ':' && *end != '?') end++;
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= cap) return false;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

// 单独做一次 DNS 解析:能把"解析失败"和"连不上"区分开,诊断才有意义。
static bool host_resolves(const char *host)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int r = getaddrinfo(host, "443", &hints, &res);
    if (res) freeaddrinfo(res);
    return r == 0;
}

static esp_err_t http_get(const char *url, char *buf, int cap, int *out_len, int *out_code)
{
    http_sink_t sink = { buf, 0, cap };
    buf[0] = 0;
    *out_code = 0;

    esp_http_client_config_t cfg = {
        .url                   = url,
        .event_handler         = http_on_data,
        .user_data             = &sink,
        .timeout_ms            = 12000,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .buffer_size           = 2048,
        .buffer_size_tx        = 1024,
        .max_redirection_count = 5,
        .keep_alive_enable     = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_err_t err = esp_http_client_perform(cli);
    *out_code = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (out_len) *out_len = sink.len;
    return err;
}

// 逐个镜像尝试。成功返回 true 并把镜像名写进 s_mirror_name。
static bool fetch_try_mirrors(void)
{
    char host[80];

    // 用户自定义源优先(整条自定义 URL 也走同一套诊断)。
    for (int pass = 0; pass < 2; pass++) {
        int n  = (pass == 0) ? 1 : (int)MIRROR_N;
        const char *url;
        for (int i = 0; i < n; i++) {
            const char *name;
            if (pass == 0) {
                const char *cu = cs_net_custom_url();
                if (!cu[0]) continue;
                url  = cu;
                name = "自定义源";
            } else {
                url  = MIRRORS[i].url;
                name = MIRRORS[i].name;
            }

            if (!url_host(url, host, sizeof(host))) { diag_set(CS_DIAG_HTTP); continue; }
            if (!host_resolves(host)) {
                ESP_LOGW(TAG, "[%s] DNS 解析失败: %s", name, host);
                diag_set(CS_DIAG_DNS);
                continue;
            }

            char *buf = (char *)malloc(CS_JSON_MAX);
            if (!buf) { diag_set(CS_DIAG_MEM); return false; }

            int len = 0, code = 0;
            ESP_LOGI(TAG, "[%s] 拉取 %s", name, url);
            esp_err_t err = http_get(url, buf, CS_JSON_MAX, &len, &code);

            bool ok = false;
            if (err == ESP_OK && code == 200 && len > 0) {
                if (cs_data_apply_json(buf, true, "net")) {
                    scpy(s_mirror_name, sizeof(s_mirror_name), name);
                    ok = true;
                    diag_set(CS_DIAG_NONE);
                    s_backoff_s = CS_BACKOFF_MIN;
                    cs_data_fetch_set(CS_FETCH_OK, NULL);
                    ESP_LOGI(TAG, "[%s] 成功: %d 字节", name, len);
                } else {
                    diag_set(CS_DIAG_PARSE);
                }
            } else if (err == ESP_ERR_HTTP_CONNECT) {
                diag_set(cs_net_time_synced() ? CS_DIAG_TCP : CS_DIAG_TIME);
                ESP_LOGW(TAG, "[%s] 连接失败: %s", name, esp_err_to_name(err));
            } else if (err == ESP_ERR_HTTP_FETCH_HEADER) {
                diag_set(CS_DIAG_TLS);
                ESP_LOGW(TAG, "[%s] TLS/首部失败: %s", name, esp_err_to_name(err));
            } else if (err != ESP_OK) {
                diag_set(CS_DIAG_TCP);
                ESP_LOGW(TAG, "[%s] 传输失败: %s", name, esp_err_to_name(err));
            } else if (code != 200) {
                diag_set(CS_DIAG_HTTP);
                ESP_LOGW(TAG, "[%s] HTTP %d", name, code);
            } else {
                diag_set(CS_DIAG_EMPTY);
                ESP_LOGW(TAG, "[%s] 空响应", name);
            }

            free(buf);
            if (ok) return true;
        }
    }
    return false;
}

static const char *diag_word(cs_diag_t d)
{
    switch (d) {
    case CS_DIAG_NO_WIFI: return "未连上 Wi-Fi";
    case CS_DIAG_TIME:    return "设备时间未同步";
    case CS_DIAG_DNS:     return "域名解析失败";
    case CS_DIAG_TCP:     return "连不上服务器";
    case CS_DIAG_TLS:     return "证书/握手失败";
    case CS_DIAG_HTTP:    return "服务器返回异常";
    case CS_DIAG_EMPTY:   return "收到空数据";
    case CS_DIAG_PARSE:   return "数据格式不对";
    case CS_DIAG_MEM:     return "内存不足";
    default:              return "正常";
    }
}

static void fetch_task(void *arg)
{
    (void)arg;

    if (!cs_net_online()) {
        diag_set(CS_DIAG_NO_WIFI);
        cs_data_fetch_set(CS_FETCH_FAIL, "未连上 Wi-Fi");
        s_fetching = false;
        s_due = CS_BACKOFF_MIN * (1000 / CS_TICK_MS);
        vTaskDelete(NULL);
        return;
    }

    // HTTPS 前置校时:不校时的话证书有效期校验必然失败,和"网络不通"表现一模一样。
    if (!wait_clock(15000)) {
        ESP_LOGW(TAG, "时钟未同步,HTTPS 可能失败(将按证书错误归类)");
    }

    bool ok = fetch_try_mirrors();

    if (ok) {
        char msg[48];
        snprintf(msg, sizeof(msg), "已更新 %d 场", cs_data()->count % 1000);
        cs_data_fetch_set(CS_FETCH_OK, msg);
        s_due = CS_BACKOFF_MAX * (1000 / CS_TICK_MS);
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s", diag_word(s_diag));
        cs_data_fetch_set(CS_FETCH_FAIL, msg);
        s_due = s_backoff_s * (1000 / CS_TICK_MS);
        if (s_backoff_s < CS_BACKOFF_MAX) {
            s_backoff_s = s_backoff_s * 2 > CS_BACKOFF_MAX ? CS_BACKOFF_MAX : s_backoff_s * 2;
        }
        ESP_LOGE(TAG, "全部镜像失败,原因: %s(下次 %ds 后重试)", diag_word(s_diag), s_backoff_s);
    }

    s_fetching = false;
    vTaskDelete(NULL);
}

void cs_net_refresh(bool force)
{
    if (s_fetching) return;
    if (!cs_net_online()) {
        diag_set(CS_DIAG_NO_WIFI);
        cs_data_fetch_set(CS_FETCH_FAIL, "未连上 Wi-Fi");
        return;
    }
    if (force) s_backoff_s = CS_BACKOFF_MIN;

    s_fetching = true;
    cs_data_fetch_set(CS_FETCH_RUNNING, "正在拉取...");
    s_due = CS_BACKOFF_MAX * (1000 / CS_TICK_MS);   // 先占住,任务结束再按结果改写
    if (xTaskCreate(fetch_task, "cs_fetch", 8192, NULL, 5, NULL) != pdPASS) {
        s_fetching = false;
        cs_data_fetch_set(CS_FETCH_FAIL, "任务创建失败");
    }
}

cs_diag_t   cs_net_diag(void)      { return s_diag; }
const char *cs_net_diag_text(void) { return diag_word(s_diag); }

// ---------------------------------------------------------------------------
// 周期维护
// ---------------------------------------------------------------------------
void cs_net_tick(void)
{
    // --- 延迟重连:断线回调只登记,真正 esp_wifi_connect() 在这里做 ---
    if (s_retry_pending > 0 && --s_retry_pending == 0) {
        s_retry_pending = -1;
        ESP_LOGI(TAG, "重连 %s(第 %d 次)", s_ssid[0] ? s_ssid : "已存网络", s_reconnect);
        esp_wifi_connect();
    }

    // --- 连接看门狗:卡在 CONNECTING 超 20s 判失败,界面永远有出口 ---
    if (s_state == CS_NET_CONNECTING) {
        if (++s_conn_watch >= 100) {
            s_conn_watch = 0;
            s_state = CS_NET_FAILED;
            s_retry_watch = 0;
            if (!s_conn_err[0]) scpy(s_conn_err, sizeof(s_conn_err), "连接超时,请重试");
            ESP_LOGW(TAG, "连接超时");
        }
    } else {
        s_conn_watch = 0;
    }

    // --- 扫描看门狗:阻塞式扫描理论上会自己返回,这里只兜底 ---
    if (s_state == CS_NET_SCANNING) {
        if (++s_scan_watch >= 75) {
            s_scan_watch = 0;
            ESP_LOGW(TAG, "扫描超时,强制收尾");
            esp_wifi_scan_stop();
            s_scan_busy = false;
            s_ap_count = 0;
            s_state = s_scan_keep_online ? CS_NET_ONLINE : CS_NET_FAILED;
            scpy(s_scan_msg, sizeof(s_scan_msg), "扫描超时,按确定重试");
        }
    } else {
        s_scan_watch = 0;
    }

    // --- 断线自动重连:FAILED 且存有凭证,每 30s 试一次(不打扰用户) ---
    if (s_state == CS_NET_FAILED && s_prev_ssid[0] && !s_scan_busy) {
        if (++s_retry_watch >= 150) {          // 200ms x 150 = 30s
            s_retry_watch = 0;
            ESP_LOGI(TAG, "自动重连 %s", s_prev_ssid);
            cs_net_autoconnect();
        }
    } else {
        s_retry_watch = 0;
    }

    // --- 自动拉取调度 ---
    if (s_due > 0) s_due--;
    if (s_due <= 0 && cs_net_online() && !s_fetching) cs_net_refresh(false);
}

// ---------------------------------------------------------------------------
// 时间(状态栏 HH:MM,北京时间)
// ---------------------------------------------------------------------------
void cs_time_hhmm(char *out, int n)
{
    if (!out || n <= 0) return;
    time_t now = time(NULL);
    if ((long long)now < CS_TIME_MIN) { snprintf(out, (size_t)n, "--:--"); return; }
    time_t bj = now + 8 * 3600;
    struct tm tmv;
    gmtime_r(&bj, &tmv);
    snprintf(out, (size_t)n, "%02d:%02d", tmv.tm_hour % 100, tmv.tm_min % 100);
}
