#include "app_data.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app_config.h"

static SemaphoreHandle_t s_lock;
static app_sensor_data_t s_sensor;
static app_alarm_thresholds_t s_thresholds;
static app_alarm_state_t s_alarm;

/* app_data 是跨任务共享数据中心，用互斥锁保护读写。 */
void app_data_init_defaults(void)
{
    static StaticSemaphore_t lock_buffer;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&lock_buffer);
    }

    s_sensor = (app_sensor_data_t) {
        .temperature_c = 0.0f,
        .humidity_rh = 0.0f,
        .co_ppm = 0.0f,
        .co_raw = 0,
        .co_mv = 0,
        .co2_ppm = 0.0f,
        .tvoc_ppb = 0.0f,
        .battery_adc_raw = 0,
        .battery_adc_mv = 0,
        .battery_mv = 0,
        .battery_percent = 0,
        .battery_charging = false,
        .battery_full = false,
    };
    s_thresholds = (app_alarm_thresholds_t) {
        .temp_high_c = APP_DEFAULT_TEMP_HIGH_C,
        .humidity_high_rh = APP_DEFAULT_HUMIDITY_HIGH_RH,
        .co_high_ppm = APP_DEFAULT_CO_HIGH_PPM,
        .co2_high_ppm = APP_DEFAULT_CO2_HIGH_PPM,
        .alarm_duration_min = APP_DEFAULT_ALARM_DURATION_MIN,
    };
    memset(&s_alarm, 0, sizeof(s_alarm));
}

void app_data_set_sensor(const app_sensor_data_t *data)
{
    if (data == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sensor = *data;
    xSemaphoreGive(s_lock);
}

void app_data_get_sensor(app_sensor_data_t *data)
{
    if (data == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *data = s_sensor;
    xSemaphoreGive(s_lock);
}

void app_data_set_thresholds(const app_alarm_thresholds_t *thresholds)
{
    if (thresholds == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_thresholds = *thresholds;
    xSemaphoreGive(s_lock);
}

void app_data_get_thresholds(app_alarm_thresholds_t *thresholds)
{
    if (thresholds == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *thresholds = s_thresholds;
    xSemaphoreGive(s_lock);
}

void app_data_set_alarm(const app_alarm_state_t *alarm)
{
    if (alarm == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_alarm = *alarm;
    xSemaphoreGive(s_lock);
}

void app_data_get_alarm(app_alarm_state_t *alarm)
{
    if (alarm == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *alarm = s_alarm;
    xSemaphoreGive(s_lock);
}
