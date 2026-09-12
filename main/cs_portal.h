// cs_portal.h —— 手机配网/数据门户(softAP + 内嵌网页)。
//
// 为什么要有它:板子自己出不了公网的原因五花八门(隔离网、captive portal、
// DNS 污染、NTP 被挡、TLS 被中间设备掐断),固件再怎么写也治不了别人的网。
// 但用户的手机几乎总是能上网。所以给板子开一个热点 + 一张网页,让**手机**去抓数据,
// 再 POST 给板子写进 Flash —— 这条路不依赖板子的联网能力。
//
// 网页还支持手动粘贴 JSON 与自定义数据源,作为手机也抓不到时的兜底。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// 启动门户(热点 + HTTP 服务器)。可重复调用。
esp_err_t cs_portal_start(void);
// 关闭门户并切回 STA 模式(会自动重连已保存的 Wi-Fi)。
void cs_portal_stop(void);
bool cs_portal_active(void);

const char *cs_portal_ssid(void);   // 热点名
const char *cs_portal_pass(void);   // 热点密码
const char *cs_portal_url(void);    // 网页地址
const char *cs_portal_msg(void);    // 最近一次手机操作的结果(上屏提示)
void        cs_portal_clear_msg(void);
