// cs_niko.h —— NiKo 专属看板入口(取代原通用 cs_app 看板)
#pragma once

#include "bsp_button.h"
#include "lvgl.h"

// 启动 NiKo 看板(内部会做数据/电量/键位就绪,并起 200ms 渲染与网络维护定时器)。
void cs_niko_start(void);

// 按键分发(在持 LVGL 锁的上下文里被 key_task 调用,必须非阻塞)。
void cs_niko_key(bsp_btn_t btn, bsp_btn_ev_t ev);
