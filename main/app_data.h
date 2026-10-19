#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* 当前传感器实时数据，后续由具体传感器驱动填充。 */
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
} app_sensor_data_t;

/* 报警阈值，启动时从 NVS 读取，没有保存值时使用默认值。 */
typedef struct {
    float temp_high_c;
    float humidity_high_rh;
    float co_high_ppm;
    float co2_high_ppm;
    uint32_t alarm_duration_min;
} app_alarm_thresholds_t;

/* 报警判断结果，UI 和蜂鸣器逻辑都可以读取。 */
typedef struct {
    bool temp_high;
    bool humidity_high;
    bool co_high;
    bool co2_high;
    bool active;
} app_alarm_state_t;

void app_data_init_defaults(void);
void app_data_set_sensor(const app_sensor_data_t *data);
void app_data_get_sensor(app_sensor_data_t *data);
void app_data_set_thresholds(const app_alarm_thresholds_t *thresholds);
void app_data_get_thresholds(app_alarm_thresholds_t *thresholds);
void app_data_set_alarm(const app_alarm_state_t *alarm);
void app_data_get_alarm(app_alarm_state_t *alarm);
