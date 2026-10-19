#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 传感器抽象接口：后续替换为真实温湿度、CO2、TVOC 驱动。 */
typedef struct {
    float temperature_c;
    float humidity_rh;
    float co_ppm;
    int co_raw;
    int co_mv;
    float co2_ppm;
    float tvoc_ppb;
    int battery_adc_raw;
    int battery_adc_mv;
    int battery_mv;
    int battery_percent;
    bool battery_charging;
    bool battery_full;
} sensor_values_t;

esp_err_t sensor_init(void);
esp_err_t sensor_read(sensor_values_t *values);
