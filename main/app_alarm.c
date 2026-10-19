#include "app_alarm.h"

#include <stdint.h>
#include "driver/gpio.h"
#include "app_config.h"
#include "app_data.h"
#include "esp_log.h"

static const char *TAG = "app_alarm";
static app_alarm_state_t s_last_alarm;
static bool s_alarm_log_initialized;

esp_err_t app_alarm_init(void)
{
    s_alarm_log_initialized = false;

    /* 当前蜂鸣器已独立用 PWM 驱动，报警 GPIO 暂未启用。 */
    gpio_num_t alarm_gpio = APP_GPIO_ALARM_OUT;
    if (alarm_gpio == GPIO_NUM_NC) {
        return ESP_OK;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << alarm_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_set_level(alarm_gpio, 0);
    }
    return ret;
}

void app_alarm_update(void)
{
    /* 每次传感器数据刷新后调用，重新计算所有报警位。 */
    app_sensor_data_t sensor;
    app_alarm_thresholds_t thresholds;
    app_data_get_sensor(&sensor);
    app_data_get_thresholds(&thresholds);

    app_alarm_state_t alarm = {
        .temp_high = sensor.temperature_c >= thresholds.temp_high_c,
        .humidity_high = sensor.humidity_rh >= thresholds.humidity_high_rh,
        /* CO 已经过 5 次中值滤波，单次滤波结果达到阈值即可报警。 */
        .co_high = sensor.co_ppm >= thresholds.co_high_ppm,
        .co2_high = sensor.co2_ppm >= thresholds.co2_high_ppm,
    };
    alarm.active = alarm.temp_high || alarm.humidity_high || alarm.co_high || alarm.co2_high;

    /* 仅在报警位发生变化时打印，记录实际值、阈值和具体触发来源。 */
    bool alarm_state_changed = !s_alarm_log_initialized ||
                               alarm.temp_high != s_last_alarm.temp_high ||
                               alarm.humidity_high != s_last_alarm.humidity_high ||
                               alarm.co_high != s_last_alarm.co_high ||
                               alarm.co2_high != s_last_alarm.co2_high ||
                               alarm.active != s_last_alarm.active;
    if (alarm_state_changed) {
        ESP_LOGI(TAG,
                 "alarm active=%d [temp=%d humi=%d co=%d co2=%d] "
                 "values: temp=%.2f/%.2fC humi=%.2f/%.2f%% "
                 "co=%.0f/%.0fppm co2=%.2f/%.2fppm",
                 alarm.active,
                 alarm.temp_high,
                 alarm.humidity_high,
                 alarm.co_high,
                 alarm.co2_high,
                 sensor.temperature_c,
                 thresholds.temp_high_c,
                 sensor.humidity_rh,
                 thresholds.humidity_high_rh,
                 sensor.co_ppm,
                 thresholds.co_high_ppm,
                 sensor.co2_ppm,
                 thresholds.co2_high_ppm);
        s_last_alarm = alarm;
        s_alarm_log_initialized = true;
    }

    app_data_set_alarm(&alarm);
    gpio_num_t alarm_gpio = APP_GPIO_ALARM_OUT;
    if (alarm_gpio != GPIO_NUM_NC) {
        gpio_set_level(alarm_gpio, alarm.active ? 1 : 0);
    }
}
