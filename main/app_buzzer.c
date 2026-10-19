#include "app_buzzer.h"

#include <stdbool.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "app_config.h"

#define APP_BUZZER_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define APP_BUZZER_LEDC_TIMER      LEDC_TIMER_0
#define APP_BUZZER_LEDC_CHANNEL    LEDC_CHANNEL_0
#define APP_BUZZER_FREQ_HZ         2700
#define APP_BUZZER_DUTY_RES        LEDC_TIMER_10_BIT
#define APP_BUZZER_DUTY_MAX        ((1U << 10) - 1)
#define APP_BUZZER_DUTY_LIMIT      ((APP_BUZZER_DUTY_MAX * 70) / 100)
#define APP_BUZZER_DEFAULT_VOLUME  100

static bool s_buzzer_ready;
static uint8_t s_volume = APP_BUZZER_DEFAULT_VOLUME;
static SemaphoreHandle_t s_buzzer_lock;

/* 音量 0~100 映射到 duty 0~70%，避免 100% 直流导致无源蜂鸣器变弱。 */
static uint32_t app_buzzer_volume_to_duty(void)
{
    if (s_volume == 0) {
        return 0;
    }

    uint32_t duty = (APP_BUZZER_DUTY_LIMIT * s_volume) / 100;
    if (duty == 0) {
        duty = 1;
    }
    return duty;
}

esp_err_t app_buzzer_init(void)
{
    static StaticSemaphore_t lock_buffer;
    s_buzzer_lock = xSemaphoreCreateMutexStatic(&lock_buffer);
    if (s_buzzer_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* LEDC 负责持续输出稳定频率，GPIO 直接拉高不能驱动无源蜂鸣器发声。 */
    ledc_timer_config_t timer_conf = {
        .speed_mode = APP_BUZZER_LEDC_MODE,
        .duty_resolution = APP_BUZZER_DUTY_RES,
        .timer_num = APP_BUZZER_LEDC_TIMER,
        .freq_hz = APP_BUZZER_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num = APP_GPIO_BEEP_EN,
        .speed_mode = APP_BUZZER_LEDC_MODE,
        .channel = APP_BUZZER_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = APP_BUZZER_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret == ESP_OK) {
        s_buzzer_ready = true;
    }
    return ret;
}

esp_err_t app_buzzer_beep(uint32_t duration_ms)
{
    if (!s_buzzer_ready || s_buzzer_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_buzzer_lock, portMAX_DELAY);

    esp_err_t ret = ledc_set_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL, app_buzzer_volume_to_duty());
    if (ret != ESP_OK) {
        xSemaphoreGive(s_buzzer_lock);
        return ret;
    }
    ret = ledc_update_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_buzzer_lock);
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    ledc_set_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL);
    xSemaphoreGive(s_buzzer_lock);
    return ESP_OK;
}

void app_buzzer_set_volume(uint8_t volume)
{
    if (volume > 100) {
        volume = 100;
    }
    s_volume = volume;
}

uint8_t app_buzzer_get_volume(void)
{
    return s_volume;
}

void app_buzzer_stop(void)
{
    if (!s_buzzer_ready || s_buzzer_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_buzzer_lock, portMAX_DELAY);
    ledc_set_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL, 0);
    ledc_update_duty(APP_BUZZER_LEDC_MODE, APP_BUZZER_LEDC_CHANNEL);
    xSemaphoreGive(s_buzzer_lock);
}
