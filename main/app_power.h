#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 电源保持模块：SW1 是硬件开机键，软件通过 SW_IO 保持或释放电源。 */
esp_err_t app_power_init(void);
esp_err_t app_power_set_hold(bool on);
esp_err_t app_power_request_shutdown(void);
bool app_power_is_key_pressed(void);
