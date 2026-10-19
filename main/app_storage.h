#pragma once

#include "esp_err.h"
#include "app_data.h"

/* NVS 存储模块：保存用户设置的报警阈值。 */
esp_err_t app_storage_init(void);
esp_err_t app_storage_load_thresholds(app_alarm_thresholds_t *thresholds);
esp_err_t app_storage_save_thresholds(const app_alarm_thresholds_t *thresholds);
