#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "app_alarm.h"
#include "app_buzzer.h"
#include "app_config.h"
#include "app_data.h"
#include "app_key.h"
#include "app_power.h"
#include "app_storage.h"
#include "app_ui.h"
#include "bsp.h"
#include "io_expander.h"
#include "sensor.h"

static const char *TAG = "main";

#define SW1_SCAN_PERIOD_MS         50
#define SW1_UI_LONG_PRESS_MS       1500
#define APP_SHUTDOWN_HOLD_MS       1500
#define APP_ENABLE_LVGL_UI          1
#define APP_BUZZER_TEST_ONLY        0
#define APP_LVGL_TEST_ONLY          0

static bool alarm_thresholds_valid(const app_alarm_thresholds_t *thresholds)
{
    return thresholds != NULL &&
           isfinite(thresholds->temp_high_c) && thresholds->temp_high_c >= -20.0f &&
           thresholds->temp_high_c <= 80.0f &&
           isfinite(thresholds->humidity_high_rh) && thresholds->humidity_high_rh >= 0.0f &&
           thresholds->humidity_high_rh <= 100.0f &&
           isfinite(thresholds->co_high_ppm) && thresholds->co_high_ppm >= 0.0f &&
           thresholds->co_high_ppm <= 500.0f &&
           isfinite(thresholds->co2_high_ppm) && thresholds->co2_high_ppm >= 400.0f &&
           thresholds->co2_high_ppm <= 5000.0f &&
           thresholds->alarm_duration_min >= 1U &&
           thresholds->alarm_duration_min <= 10U;
}

static void sensor_task(void *arg)
{
    /* 周期读取传感器，并立即刷新报警状态。 */
    (void)arg;

    while (true) {
        sensor_values_t values;
        if (sensor_read(&values) == ESP_OK) {
            app_sensor_data_t data = {
                .temperature_c = values.temperature_c,
                .humidity_rh = values.humidity_rh,
                .co_ppm = values.co_ppm,
                .co_raw = values.co_raw,
                .co_mv = values.co_mv,
                .co2_ppm = values.co2_ppm,
                .tvoc_ppb = values.tvoc_ppb,
                .battery_adc_raw = values.battery_adc_raw,
                .battery_adc_mv = values.battery_adc_mv,
                .battery_mv = values.battery_mv,
                .battery_percent = values.battery_percent,
                .battery_charging = values.battery_charging,
                .battery_full = values.battery_full,
            };
            app_data_set_sensor(&data);
#if APP_ENABLE_ALARM
            app_alarm_update();
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(APP_SENSOR_TASK_PERIOD_MS));
    }
}

static void buzzer_alarm_task(void *arg)
{
    (void)arg;
    bool alarm_episode_active = false;
    int64_t alarm_start_us = 0;
    int64_t alarm_duration_us = 0;

    while (true) {
        app_alarm_state_t alarm;
        app_data_get_alarm(&alarm);
        if (!alarm.active) {
            if (alarm_episode_active) {
                app_buzzer_stop();
                alarm_episode_active = false;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (!alarm_episode_active) {
            app_alarm_thresholds_t thresholds;
            app_data_get_thresholds(&thresholds);
            uint32_t duration_min = thresholds.alarm_duration_min;
            if (duration_min < 1U) duration_min = 1U;
            if (duration_min > 10U) duration_min = 10U;
            alarm_duration_us = (int64_t)duration_min * 60LL * 1000000LL;
            alarm_start_us = esp_timer_get_time();
            alarm_episode_active = true;
            ESP_LOGI(TAG, "alarm buzzer started, duration=%umin", (unsigned)duration_min);
        }

        if (esp_timer_get_time() - alarm_start_us >= alarm_duration_us) {
            app_buzzer_stop();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 报警节奏：滴，停，滴滴，停，然后重复。 */
        app_buzzer_beep(100);
        vTaskDelay(pdMS_TO_TICKS(150));
        app_buzzer_beep(100);
        vTaskDelay(pdMS_TO_TICKS(100));
        app_buzzer_beep(100);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
}

#if APP_ENABLE_LVGL_UI
static void ui_task(void *arg)
{
    /* LVGL 需要持续调用 lv_timer_handler，按键切页和后续刷新都依赖它。 */
    (void)arg;

    while (true) {
        app_ui_update();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#endif

static void key_task(void *arg)
{
    /* ADC 按键事件任务，只处理事件，避免按住时刷状态日志。 */
    (void)arg;

    while (true) {
        app_key_event_t event;
        if (app_key_get_event(&event)) {
            app_ui_handle_key_event(&event);
        }
        vTaskDelay(pdMS_TO_TICKS(APP_KEY_TASK_PERIOD_MS));
    }
}

#if APP_BUZZER_TEST_ONLY
static void buzzer_test_key_task(void *arg)
{
    (void)arg;

    while (true) {
        app_key_event_t event;
        if (app_key_get_event(&event)) {
            if (event.type == APP_KEY_EVENT_SHORT_PRESS && event.key == APP_KEY_SW2) {
                esp_err_t ret = app_buzzer_beep(200);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "buzzer test failed: %s", esp_err_to_name(ret));
                }
            } else if (event.type == APP_KEY_EVENT_SHORT_PRESS && event.key == APP_KEY_SW3) {
                app_buzzer_stop();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(APP_KEY_TASK_PERIOD_MS));
    }
}
#endif

static void power_key_task(void *arg)
{
    /* SW1 独立于 ADC 按键，通过 IO 扩展器 KEY_CHECK 检测。 */
    (void)arg;
    bool last_pressed = app_power_is_key_pressed();
    uint32_t hold_ms = 0;
    bool long_sent = false;
    bool shutdown_latched = false;
    bool shutdown_combo_seen = false;

    while (true) {
        bool pressed = app_power_is_key_pressed();
        bool sw23_pressed = app_key_is_sw23_pressed();
        if (pressed != last_pressed) {
            ESP_LOGI(TAG, "SW1 %s", pressed ? "pressed" : "released");
            last_pressed = pressed;
            if (pressed) {
                hold_ms = 0;
                long_sent = false;
                shutdown_latched = false;
                shutdown_combo_seen = false;
            } else {
                if (!long_sent) {
                    app_key_event_t event = {
                        .type = APP_KEY_EVENT_SHORT_PRESS,
                        .key = APP_KEY_SW1,
                        .raw = -1,
                    };
                    app_ui_handle_key_event(&event);
                }
                hold_ms = 0;
                shutdown_latched = false;
                shutdown_combo_seen = false;
                long_sent = false;
            }
        }

        if (pressed && sw23_pressed) {
            shutdown_combo_seen = true;
        }

        /* SW23 长按状态与 SW1 同时成立时，表示三个按键一起长按。 */
        if (pressed && !shutdown_latched &&
            app_key_is_sw23_long_pressed(APP_SHUTDOWN_HOLD_MS)) {
            shutdown_latched = true;
            ESP_LOGI(TAG, "SW1+SW2+SW3 held for %d ms, shutting down", APP_SHUTDOWN_HOLD_MS);
            app_ui_prepare_shutdown();
            app_buzzer_stop();
            app_power_request_shutdown();
        }

        if (pressed && !long_sent) {
            hold_ms += SW1_SCAN_PERIOD_MS;
            if (hold_ms >= SW1_UI_LONG_PRESS_MS && !shutdown_combo_seen) {
                app_key_event_t event = {
                    .type = APP_KEY_EVENT_LONG_PRESS,
                    .key = APP_KEY_SW1,
                    .raw = -1,
                };
                app_ui_handle_key_event(&event);
                long_sent = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SW1_SCAN_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "IEQ Monitor starting");

#if APP_BUZZER_TEST_ONLY
    /* 独立蜂鸣器测试：暂时停用传感器、LCD、LVGL、报警、存储和电源逻辑。 */
    ESP_ERROR_CHECK(app_buzzer_init());
    ESP_ERROR_CHECK(app_key_init());
    ESP_LOGI(TAG, "buzzer test mode: SW2 beep, SW3 stop");
    xTaskCreate(buzzer_test_key_task, "buzzer_test_key", 2048, NULL, 3, NULL);
    return;
#endif

#if APP_LVGL_TEST_ONLY
    /* LVGL 独立测试：保留 LCD、LVGL、ADC 按键和蜂鸣器，停用传感器及其他业务。 */
    app_data_init_defaults();
    ESP_ERROR_CHECK(io_expander_init());
    ESP_ERROR_CHECK(bsp_io_init());
    ESP_ERROR_CHECK(bsp_lcd_init());
    ESP_ERROR_CHECK(app_buzzer_init());
    ESP_ERROR_CHECK(app_key_init());
    ESP_ERROR_CHECK(sensor_init());
    ESP_ERROR_CHECK(app_ui_init());
    ESP_LOGI(TAG, "LVGL test mode: sensors and alarm disabled");
    xTaskCreate(ui_task, "ui_task", 8192, NULL, 4, NULL);
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(key_task, "key_task", 3072, NULL, 3, NULL);
    return;
#endif

    app_data_init_defaults();

    ESP_ERROR_CHECK(app_storage_init());
    app_alarm_thresholds_t thresholds;
    if (app_storage_load_thresholds(&thresholds) == ESP_OK &&
        alarm_thresholds_valid(&thresholds)) {
        app_data_set_thresholds(&thresholds);
        ESP_LOGI(TAG, "loaded saved alarm thresholds: temp=%.1f humi=%.0f co=%.0f co2=%.0f alarm=%umin",
                 thresholds.temp_high_c, thresholds.humidity_high_rh,
                 thresholds.co_high_ppm, thresholds.co2_high_ppm,
                 (unsigned)thresholds.alarm_duration_min);
    } else {
        /* 没有保存值或保存值非法时，使用 app_config.h 中的固定默认值。 */
        app_data_get_thresholds(&thresholds);
        ESP_ERROR_CHECK(app_storage_save_thresholds(&thresholds));
        ESP_LOGW(TAG, "saved alarm thresholds missing or invalid, using defaults: temp=%.1f humi=%.0f co=%.0f co2=%.0f alarm=%umin",
                 thresholds.temp_high_c, thresholds.humidity_high_rh,
                 thresholds.co_high_ppm, thresholds.co2_high_ppm,
                 (unsigned)thresholds.alarm_duration_min);
    }

    esp_err_t ret = io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "io expander init failed: %s, continue without expander", esp_err_to_name(ret));
    }
    ret = app_power_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "power init failed: %s, continue without power control", esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK(bsp_io_init());
    ESP_LOGI(TAG, "lcd backlight brightness=%u", bsp_lcd_get_backlight_brightness());
    ret = bsp_lcd_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "lcd init failed: %s, continue without lcd", esp_err_to_name(ret));
    }
    ESP_ERROR_CHECK(sensor_init());
    ESP_ERROR_CHECK(app_alarm_init());
    ESP_ERROR_CHECK(app_buzzer_init());
    ESP_ERROR_CHECK(app_key_init());
 #if APP_ENABLE_LVGL_UI
    ESP_ERROR_CHECK(app_ui_init());
 #endif

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
#if APP_ENABLE_ALARM
    xTaskCreate(buzzer_alarm_task, "buzzer_alarm_task", 2048, NULL, 4, NULL);
#endif
 #if APP_ENABLE_LVGL_UI
    xTaskCreate(ui_task, "ui_task", 8192, NULL, 4, NULL);
 #endif
    xTaskCreate(key_task, "key_task", 3072, NULL, 3, NULL);
    xTaskCreate(power_key_task, "power_key_task", 3072, NULL, 3, NULL);
}
