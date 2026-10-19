#include "app_key.h"

#include <stdbool.h>
#include "adc_bus.h"
#include "app_config.h"
#include "esp_log.h"

#define APP_KEY_SW2_MAX_RAW       650
#define APP_KEY_SW3_MIN_RAW       700
#define APP_KEY_SW3_MAX_RAW       1400
#define APP_KEY_SW23_MIN_RAW      280
#define APP_KEY_SW23_MAX_RAW      360
#define APP_KEY_NONE_MIN_RAW      2500
#define APP_KEY_DEBOUNCE_COUNT    2
#define APP_KEY_LONG_PRESS_MS     1000
#define APP_KEY_REPEAT_MS         300

static int s_last_raw = -1;
static app_key_t s_last_sample = APP_KEY_NONE;
static app_key_t s_stable_key = APP_KEY_NONE;
static uint8_t s_same_count;
static uint32_t s_hold_ms;
static uint32_t s_repeat_ms;
static int s_press_raw = -1;
static bool s_long_sent;
static int s_last_logged_raw = -1;

static const char *TAG = "app_key";

esp_err_t app_key_init(void)
{
    /* ADC_IN 电阻分压实测：无按键约 4095，SW2 约 408，SW3 约 1035，SW2+SW3 约 313。 */
    return adc_bus_config_channel(APP_KEY_ADC_CHANNEL);
}

static app_key_t app_key_decode_raw(int raw)
{
    /* 阈值按实际板子测到的 raw 值留出余量。 */
    if (raw >= APP_KEY_NONE_MIN_RAW) {
        return APP_KEY_NONE;
    } else if (raw >= APP_KEY_SW23_MIN_RAW && raw <= APP_KEY_SW23_MAX_RAW) {
        return APP_KEY_SW23;
    } else if (raw <= APP_KEY_SW2_MAX_RAW) {
        return APP_KEY_SW2;
    } else if (raw >= APP_KEY_SW3_MIN_RAW && raw <= APP_KEY_SW3_MAX_RAW) {
        return APP_KEY_SW3;
    }
    return APP_KEY_NONE;
}

app_key_t app_key_scan(void)
{
    int raw = 0;
    if (adc_bus_read_raw(APP_KEY_ADC_CHANNEL, &raw) != ESP_OK) {
        return APP_KEY_NONE;
    }
    s_last_raw = raw;

    /* 调试 SW2/SW3 电阻分压；仅在 ADC 原始值变化时打印，便于测量组合按键。 */
    if (raw != s_last_logged_raw) {
        ESP_LOGI(TAG, "SW23 ADC raw=%d", raw);
        s_last_logged_raw = raw;
    }

    app_key_t sample = app_key_decode_raw(raw);
    if (sample == s_last_sample) {
        if (s_same_count < APP_KEY_DEBOUNCE_COUNT) {
            s_same_count++;
        }
    } else {
        s_last_sample = sample;
        s_same_count = 1;
    }

    if (s_same_count >= APP_KEY_DEBOUNCE_COUNT) {
        s_stable_key = sample;
    }

    return s_stable_key;
}

bool app_key_get_event(app_key_event_t *event)
{
    /* 短按在松手时触发；长按 1s 触发，之后按 300ms 重复。 */
    if (event == NULL) {
        return false;
    }

    event->type = APP_KEY_EVENT_NONE;
    event->key = APP_KEY_NONE;
    event->raw = s_last_raw;

    app_key_t previous_key = s_stable_key;
    app_key_t current_key = app_key_scan();

    if (previous_key == APP_KEY_NONE && current_key != APP_KEY_NONE) {
        s_hold_ms = 0;
        s_repeat_ms = 0;
        s_press_raw = s_last_raw;
        s_long_sent = false;
        return false;
    }

    if (previous_key != APP_KEY_NONE && current_key == APP_KEY_NONE) {
        if (!s_long_sent) {
            event->type = APP_KEY_EVENT_SHORT_PRESS;
            event->key = previous_key;
            event->raw = s_press_raw;
            return true;
        }
        s_hold_ms = 0;
        s_repeat_ms = 0;
        s_long_sent = false;
        return false;
    }

    if (current_key != APP_KEY_NONE) {
        s_hold_ms += APP_KEY_TASK_PERIOD_MS;
        s_repeat_ms += APP_KEY_TASK_PERIOD_MS;

        if (!s_long_sent && s_hold_ms >= APP_KEY_LONG_PRESS_MS) {
            s_long_sent = true;
            s_repeat_ms = 0;
            event->type = APP_KEY_EVENT_LONG_PRESS;
            event->key = current_key;
            event->raw = s_press_raw;
            return true;
        }

        if (s_long_sent && s_repeat_ms >= APP_KEY_REPEAT_MS) {
            s_repeat_ms = 0;
            event->type = APP_KEY_EVENT_REPEAT;
            event->key = current_key;
            event->raw = s_press_raw;
            return true;
        }
    }

    return false;
}

int app_key_get_last_raw(void)
{
    return s_last_raw;
}

bool app_key_is_sw23_pressed(void)
{
    return s_stable_key == APP_KEY_SW23;
}

bool app_key_is_sw23_long_pressed(uint32_t hold_ms)
{
    return s_stable_key == APP_KEY_SW23 && s_hold_ms >= hold_ms;
}

const char *app_key_name(app_key_t key)
{
    switch (key) {
    case APP_KEY_SW1:
        return "SW1";
    case APP_KEY_SW2:
        return "SW2";
    case APP_KEY_SW3:
        return "SW3";
    case APP_KEY_SW23:
        return "SW23";
    case APP_KEY_NONE:
    default:
        return "NONE";
    }
}

const char *app_key_event_name(app_key_event_type_t type)
{
    switch (type) {
    case APP_KEY_EVENT_SHORT_PRESS:
        return "SHORT";
    case APP_KEY_EVENT_LONG_PRESS:
        return "LONG";
    case APP_KEY_EVENT_REPEAT:
        return "REPEAT";
    case APP_KEY_EVENT_NONE:
    default:
        return "NONE";
    }
}
