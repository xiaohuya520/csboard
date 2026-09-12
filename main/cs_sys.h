// cs_sys.h —— 全局子系统的一次性准备(NVS / netif / 默认事件循环)。
//
// 这些初始化按应用生命周期只做一次,失败不在里面擦除用户数据。Wi-Fi、SNTP、
// HTTP 服务器、以及数据缓存分区都依赖它们先就绪。
#pragma once

#include "esp_err.h"
#include <stdbool.h>

// NVS(默认分区)。幂等。
esp_err_t cs_sys_nvs(void);

// esp_netif + 默认事件循环。幂等。
esp_err_t cs_sys_netif(void);
