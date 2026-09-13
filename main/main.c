// main/main.c —— 启动流程:板级初始化 -> 载入缓存 -> 按键任务 -> 看板 -> 联网。
//
// 与旧工程最大的不同:这里没有 FoloToy 官方 demo 菜单,整块屏都是 CS 看板自己管。
// 少一层页面框架,就少一处"长按返回到底该回哪"的耦合。
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"

#include "cs_niko.h"
#include "cs_data.h"
#include "cs_net.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

// 按键回调跑在 button 组件的任务里,只做入队。真正的处理放在自带 6KB 栈的
// key_task 里 —— 界面重建(几十个 LVGL 对象)绝不在组件任务的栈上做。
typedef struct {
    uint8_t btn;
    uint8_t ev;
} key_evt_t;

static QueueHandle_t s_keyq;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_keyq) return;
    key_evt_t e = { (uint8_t)btn, (uint8_t)ev };
    xQueueSend(s_keyq, &e, 0);
}

static void key_task(void *arg)
{
    (void)arg;
    key_evt_t e;
    for (;;) {
        if (xQueueReceive(s_keyq, &e, portMAX_DELAY) != pdTRUE) continue;
        if (!bsp_lvgl_lock(1000)) continue;
        cs_niko_key((bsp_btn_t)e.btn, (bsp_btn_ev_t)e.ev);
        bsp_lvgl_unlock();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "CS 赛事看板启动");

    bsp_i2c_init();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,检查 SPI 接线与屏型号");
        return;
    }
    bsp_display_backlight(100);

    // 离线优先:先把 Flash 缓存读进模型,断网也能立刻看到上次的赛程。
    cs_data_load_persisted();

    // 按键先能收:队列 + 分发任务就位后再注册回调。
    s_keyq = xQueueCreate(10, sizeof(key_evt_t));
    if (s_keyq && xTaskCreate(key_task, "ui_key", 6144, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_keyq);
        s_keyq = NULL;
        ESP_LOGE(TAG, "按键任务创建失败");
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    if (bsp_lvgl_lock(1000)) {
        cs_niko_start();
        bsp_lvgl_unlock();
    }

    // 联网:有凭证直接重连,没有则扫描;失败会自动退避重试(见 cs_net_tick)。
    cs_net_init();

    ESP_LOGI(TAG, "就绪,堆余量 %u 字节", (unsigned)esp_get_free_heap_size());
}
