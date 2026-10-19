#pragma once

#include "esp_err.h"

/* 报警模块：根据实时数据和阈值更新报警状态。 */
esp_err_t app_alarm_init(void);
void app_alarm_update(void);
