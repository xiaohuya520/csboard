// cs_portal.c —— softAP + 内嵌设置网页(手机代理抓取 / 粘贴 JSON / 换源 / 直连 Wi-Fi)。
#include "cs_portal.h"
#include "cs_data.h"
#include "cs_net.h"
#include "cs_sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cs_portal";

#define PORTAL_SSID_PREFIX "CSBoard-"
#define PORTAL_PASS        "12345678"
#define PORTAL_URL         "http://192.168.4.1"
#define BODY_MAX           20480

static httpd_handle_t s_srv;
static esp_netif_t   *s_ap;
static bool           s_active;
static char           s_ssid[33];
static char           s_msg[64];

static void scpy(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

const char *cs_portal_ssid(void) { return s_ssid; }
const char *cs_portal_pass(void) { return PORTAL_PASS; }
const char *cs_portal_url(void)  { return PORTAL_URL; }
const char *cs_portal_msg(void)  { return s_msg; }
void        cs_portal_clear_msg(void) { s_msg[0] = 0; }
bool        cs_portal_active(void)    { return s_active; }

// ---------------------------------------------------------------------------
// 请求体 / 表单工具
// ---------------------------------------------------------------------------
static char *read_body(httpd_req_t *r)
{
    int total = (int)r->content_len;
    if (total <= 0 || total > BODY_MAX) return NULL;

    char *buf = (char *)malloc((size_t)total + 1);
    if (!buf) return NULL;

    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(r, buf + got, (size_t)(total - got));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[got] = 0;
    return buf;
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

// 从 application/x-www-form-urlencoded 主体里取一个字段(会解码)。
static bool form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!body || !key) return false;
    size_t klen = strlen(key);
    const char *p = body;
    while ((p = strstr(p, key)) != NULL) {
        // 要求是字段名(前一个字符是串首或 &),避免 "url" 命中 "curl" 之类。
        if (p == body || p[-1] == '&') {
            if (p[klen] == '=') {
                const char *v = p + klen + 1;
                const char *end = strchr(v, '&');
                size_t n = end ? (size_t)(end - v) : strlen(v);
                if (n >= cap) n = cap - 1;
                memcpy(out, v, n);
                out[n] = 0;
                url_decode(out);
                return true;
            }
        }
        p += klen;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
static const char PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>CS 看板设置</title><style>"
"body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;margin:0;padding:14px;"
"background:#12161c;color:#e8eef5}h1{font-size:19px;margin:0 0 10px}"
"p{margin:5px 0;font-size:14px;line-height:1.55}b{color:#fff}"
".card{background:#1b2129;border-radius:10px;padding:12px;margin:10px 0}"
".k{color:#8fa3b8}textarea{width:100%;height:120px;box-sizing:border-box;background:#0d1117;"
"color:#cfe3ff;border:1px solid #2c3644;border-radius:8px;padding:8px;font-size:12px}"
"button{font-size:15px;padding:10px 14px;margin:6px 6px 0 0;border:0;border-radius:8px;"
"background:#2f81f7;color:#fff}button.g{background:#2ea043}button.d{background:#444c56}"
"input{width:100%;box-sizing:border-box;padding:9px;margin:4px 0;background:#0d1117;color:#fff;"
"border:1px solid #2c3644;border-radius:8px}#msg{font-size:13px;color:#f0c674;min-height:18px}"
"</style></head><body><h1>CS 看板 · 手机设置台</h1>"
"<div class='card'><p><span class='k'>Wi-Fi:</span> <b id='ssid'>--</b></p>"
"<p><span class='k'>数据来源:</span> <b id='src'>--</b> <span class='k'>场次:</span> <b id='cnt'>--</b></p>"
"<p><span class='k'>数据时间:</span> <span id='upd'>--</span> "
"<span class='k'>拉取:</span> <span id='fe'>--</span></p>"
"<p><span class='k'>诊断:</span> <span id='diag'>--</span></p></div>"
"<div class='card'><p><b>① 一键更新</b>(用手机的网络抓取,再写给看板)</p>"
"<button class='g' onclick='grab()'>抓取并写入</button>"
"<button class='d' onclick='stat()'>刷新状态</button><p id='msg'></p></div>"
"<div class='card'><p><b>② 手动粘贴 JSON</b>(手机也抓不到时用这条)</p>"
"<textarea id='jx' placeholder='把 cs_matches.json 的内容整段粘进来'></textarea>"
"<button onclick='post_j()'>写入看板</button>"
"<button class='d' onclick='clr()'>清空</button></div>"
"<div class='card'><p><b>③ 换数据源</b>(默认走内置镜像列表)</p>"
"<input id='u' placeholder='https://.../cs_matches.json'>"
"<button onclick='set_u()'>保存</button>"
"<button class='d' onclick='rst_u()'>恢复默认</button></div>"
"<div class='card'><p><b>④ 直连 Wi-Fi</b></p>"
"<button onclick='scan()'>扫描附近网络</button><div id='aps'></div></div>"
"<script>"
"var URL='';"
"function $(i){return document.getElementById(i);}"
"function txt(i,s){$(i).textContent=s;}"
"function get(u,cb){fetch(u,{cache:'no-store'}).then(function(r){return r.text()}).then(cb)"
".catch(function(e){txt('msg','读取失败');});}"
"function post(u,b,cb){fetch(u,{method:'POST',body:b}).then(function(r){return r.text()}).then(cb)"
".catch(function(e){txt('msg','提交失败');});}"
"function stat(){get('/status',function(t){try{var o=JSON.parse(t);"
"txt('ssid',o.ssid||'(未连接)');txt('src',o.source||'-');txt('cnt',o.count);"
"txt('upd',o.updated||'-');txt('fe',o.fetch||'-');txt('diag',o.diag||'-');"
"if(o.url){URL=o.url;$('u').value=o.url;}}catch(e){}});}"
"function grab(){txt('msg','正在用手机抓取…');"
"fetch(URL,{cache:'no-store'}).then(function(r){return r.text()}).then(function(t){"
"if(t.indexOf('matches')<0){txt('msg','拿到的不是赛事数据,请用②手动粘贴');return;}"
"$('jx').value=t;post('/inject',t,function(r){txt('msg','已写入 '+r);stat();});})"
".catch(function(e){txt('msg','手机也抓不到,请用②手动粘贴');});}"
"function post_j(){var t=$('jx').value;if(!t){txt('msg','内容为空');return;}"
"post('/inject',t,function(r){txt('msg','已写入 '+r);stat();});}"
"function clr(){$('jx').value='';txt('msg','');}"
"function set_u(){post('/url','url='+encodeURIComponent($('u').value),function(r){txt('msg',r);stat();});}"
"function rst_u(){$('u').value='';post('/url','url=',function(r){txt('msg',r);stat();});}"
"function scan(){txt('msg','扫描中…');get('/scan',function(t){var a=[];try{a=JSON.parse(t)}catch(e){}"
"var box=$('aps');box.innerHTML='';"
"for(var i=0;i<a.length;i++){(function(o){var b=document.createElement('button');"
"b.textContent=o.ssid+' ('+o.rssi+'dBm)';if(!o.sec){b.className='g';}"
"b.onclick=function(){cn(o.ssid,o.sec)};box.appendChild(b);})(a[i]);}"
"txt('msg','共 '+a.length+' 个网络');});}"
"function cn(s,sec){var p='';if(sec){p=prompt('请输入密码');if(p===null){return;}}"
"post('/connect','ssid='+encodeURIComponent(s)+'&pw='+encodeURIComponent(p),"
"function(r){txt('msg',r);stat();});}"
"stat();setInterval(stat,4000);"
"</script></body></html>";

static esp_err_t send_json_err(httpd_req_t *r, const char *msg)
{
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_set_status(r, "400 Bad Request");
    return httpd_resp_sendstr(r, msg);
}

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *r)
{
    const cs_data_t *d = cs_data();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"source\":\"%s\",\"count\":%d,\"updated\":\"%s\","
             "\"fetch\":\"%s\",\"diag\":\"%s\",\"url\":\"%s\"}",
             cs_net_ssid(), d->source, d->count, d->updated,
             cs_data_fetch_msg(), cs_net_diag_text(), cs_net_url_now());
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

static esp_err_t h_inject(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) return send_json_err(r, "内容为空或过大");

    bool ok = cs_data_apply_json(body, true, "手机");
    free(body);

    if (!ok) return send_json_err(r, "不是有效的赛事 JSON");

    const cs_data_t *d = cs_data();
    char msg[48];
    snprintf(msg, sizeof(msg), "%d 场赛事", d->count % 1000);
    scpy(s_msg, sizeof(s_msg), "手机写入成功");
    cs_data_fetch_set(CS_FETCH_OK, "手机写入");
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, msg);
}

static esp_err_t h_url(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) return send_json_err(r, "缺少内容");

    char url[192];
    if (!form_get(body, "url", url, sizeof(url))) { free(body); return send_json_err(r, "缺少 url 字段"); }
    free(body);

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return send_json_err(r, "地址需以 http(s):// 开头");
    }
    cs_net_set_custom_url(url);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, url[0] ? "数据源已保存" : "已恢复默认数据源");
}

static esp_err_t h_refresh(httpd_req_t *r)
{
    cs_net_refresh(true);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, "已开始拉取");
}

// 安全拼接:不依赖 snprintf 的返回值,天然规避 -Wformat-truncation。
typedef struct { char *b; size_t cap; size_t len; } sbuf_t;

static void sb_puts(sbuf_t *s, const char *t)
{
    if (!s || !t || s->cap == 0) return;
    size_t n = strlen(t);
    if (s->len + n > s->cap - 1) n = s->cap - 1 - s->len;
    memcpy(s->b + s->len, t, n);
    s->len += n;
    s->b[s->len] = 0;
}

static void sb_puti(sbuf_t *s, int v)
{
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    sb_puts(s, t);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    // 阻塞式扫描,最多等 9s;单用户场景够用,也让网页逻辑简单。
    cs_net_scan();
    for (int i = 0; i < 45 && cs_net_scan_busy(); i++) vTaskDelay(pdMS_TO_TICKS(200));

    char buf[900];
    buf[0] = 0;
    sbuf_t sb = { buf, sizeof(buf), 0 };

    sb_puts(&sb, "[");
    for (int i = 0; i < cs_net_ap_count(); i++) {
        sb_puts(&sb, i ? ",{\"ssid\":\"" : "{\"ssid\":\"");
        sb_puts(&sb, cs_net_ap_ssid(i));
        sb_puts(&sb, "\",\"rssi\":");
        sb_puti(&sb, cs_net_ap_rssi(i));
        sb_puts(&sb, ",\"sec\":");
        sb_puti(&sb, cs_net_ap_secure(i) ? 1 : 0);
        sb_puts(&sb, "}");
    }
    sb_puts(&sb, "]");

    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, buf);
}

static esp_err_t h_connect(httpd_req_t *r)
{
    char *body = read_body(r);
    if (!body) return send_json_err(r, "缺少内容");

    char ssid[64] = { 0 }, pw[80] = { 0 };
    bool have = form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pw", pw, sizeof(pw));
    free(body);
    if (!have || !ssid[0]) return send_json_err(r, "缺少 ssid");

    cs_net_connect(ssid, pw);
    for (int i = 0; i < 60; i++) {
        if (cs_net_online() || cs_net_state() == CS_NET_FAILED) break;
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    char msg[96];
    if (cs_net_online()) snprintf(msg, sizeof(msg), "已连接 %s,IP %s", cs_net_ssid(), cs_net_ip());
    else snprintf(msg, sizeof(msg), "连接失败: %s", cs_net_conn_err());
    scpy(s_msg, sizeof(s_msg), msg);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(r, msg);
}

// ---------------------------------------------------------------------------
// 启停
// ---------------------------------------------------------------------------
static void ssid_from_mac(void)
{
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    snprintf(s_ssid, sizeof(s_ssid), PORTAL_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
}

esp_err_t cs_portal_start(void)
{
    if (s_active) return ESP_OK;

    if (cs_sys_nvs() != ESP_OK) return ESP_FAIL;
    if (cs_sys_netif() != ESP_OK) return ESP_FAIL;

    if (!s_ap) {
        s_ap = esp_netif_create_default_wifi_ap();
        if (!s_ap) return ESP_ERR_NO_MEM;
    }
    ssid_from_mac();

    // APSTA:既能给手机开热点,又不影响板子自己连路由器(能连就自动更新)。
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) ESP_LOGW(TAG, "set_mode APSTA: %s", esp_err_to_name(err));

    wifi_config_t apc;
    memset(&apc, 0, sizeof(apc));
    scpy((char *)apc.ap.ssid, sizeof(apc.ap.ssid), s_ssid);
    apc.ap.ssid_len = (uint8_t)strlen(s_ssid);
    scpy((char *)apc.ap.password, sizeof(apc.ap.password), PORTAL_PASS);
    apc.ap.channel = 6;
    apc.ap.max_connection = 2;
    apc.ap.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_AP, &apc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP 配置失败: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "AP 启动失败: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_srv) {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.max_uri_handlers = 8;
        hc.lru_purge_enable = true;
        // 字段名是 recv_wait_timeout / send_wait_timeout(秒),不是 recv_timeout_sec。
        // 手机 POST 一整份 JSON 时读得慢,给到 10s。
        hc.recv_wait_timeout = 10;
        hc.send_wait_timeout = 10;
        hc.stack_size = 8192;
        if (httpd_start(&s_srv, &hc) != ESP_OK) {
            s_srv = NULL;
            ESP_LOGE(TAG, "HTTP 服务器启动失败");
            return ESP_FAIL;
        }
        const httpd_uri_t uris[] = {
            { .uri = "/",        .method = HTTP_GET,  .handler = h_root    },
            { .uri = "/status",  .method = HTTP_GET,  .handler = h_status  },
            { .uri = "/inject",  .method = HTTP_POST, .handler = h_inject  },
            { .uri = "/url",     .method = HTTP_POST, .handler = h_url     },
            { .uri = "/refresh", .method = HTTP_POST, .handler = h_refresh },
            { .uri = "/scan",    .method = HTTP_GET,  .handler = h_scan    },
            { .uri = "/connect", .method = HTTP_POST, .handler = h_connect },
        };
        for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_srv, &uris[i]);
    }

    s_active = true;
    s_msg[0] = 0;
    ESP_LOGI(TAG, "门户已启动: %s / %s -> %s", s_ssid, PORTAL_PASS, PORTAL_URL);
    return ESP_OK;
}

void cs_portal_stop(void)
{
    if (!s_active) return;
    if (s_srv) {
        httpd_stop(s_srv);
        s_srv = NULL;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_active = false;
    ESP_LOGI(TAG, "门户已关闭,切回 STA");
    cs_net_autoconnect();
}
