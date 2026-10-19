#pragma once

#include "esp_err.h"
#include "app_key.h"

/* UI module: LVGL monitor and settings screens. */
esp_err_t app_ui_init(void);
void app_ui_update(void);
void app_ui_handle_key_event(const app_key_event_t *event);
void app_ui_prepare_shutdown(void);
