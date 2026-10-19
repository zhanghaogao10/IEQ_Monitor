#pragma once

#include <stdint.h>
#include "esp_err.h"

/* 无源蜂鸣器：使用 LEDC PWM 输出方波，音量映射到占空比。 */
esp_err_t app_buzzer_init(void);
esp_err_t app_buzzer_beep(uint32_t duration_ms);
void app_buzzer_set_volume(uint8_t volume);
uint8_t app_buzzer_get_volume(void);
void app_buzzer_stop(void);
