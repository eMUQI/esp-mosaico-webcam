// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/** @brief Initialize the native-orientation panel and its single LVGL producer task. */
esp_err_t webcam_display_start(void);
bool webcam_display_lock(int timeout_ms);
void webcam_display_unlock(void);
lv_indev_t *webcam_display_get_input_dev(void);
unsigned webcam_display_get_rotation(void);
/** @brief Apply a quarter-turn rotation; call outside the display task without its lock held. */
esp_err_t webcam_display_set_rotation(unsigned rotation);
