#include "cs_sys.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "cs_sys";

static bool s_nvs;
static bool s_netif;
static bool s_loop;

esp_err_t cs_sys_nvs(void)
{
    if (s_nvs) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 默认分区确实坏了才擦;其它错误一律不擦(可能存着卡 id 相关数据)。
        ESP_LOGW(TAG, "NVS 需要重建: %s", esp_err_to_name(err));
        if (nvs_flash_erase() != ESP_OK || (err = nvs_flash_init()) != ESP_OK) {
            ESP_LOGE(TAG, "NVS 重建失败: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }
    s_nvs = true;
    return ESP_OK;
}

esp_err_t cs_sys_netif(void)
{
    if (!s_netif) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) return err;
        s_netif = true;
    }
    if (!s_loop) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
        s_loop = true;
    }
    return ESP_OK;
}
