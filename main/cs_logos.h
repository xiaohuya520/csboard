// cs_logos.h -- team logo lookup. Call cs_logos_init() once at boot.
#pragma once

#include "lvgl.h"
#include <stdbool.h>

void cs_logos_init(void);

// 48x48 for score cards, 20x20 for list rows. NULL if the id is unknown.
const lv_image_dsc_t *cs_logo_get(const char *id);
const lv_image_dsc_t *cs_logo_get_small(const char *id);
