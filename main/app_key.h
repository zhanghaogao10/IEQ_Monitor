#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ADC 两按键：SW2、SW3 及其组合；SW1 通过 IO 扩展器独立检测。 */
typedef enum {
    APP_KEY_NONE = 0,
    APP_KEY_SW1,
    APP_KEY_SW2,
    APP_KEY_SW3,
    APP_KEY_SW23,
} app_key_t;

/* 上层只关心按键事件，设置页面可以直接消费这些事件。 */
typedef enum {
    APP_KEY_EVENT_NONE = 0,
    APP_KEY_EVENT_SHORT_PRESS,
    APP_KEY_EVENT_LONG_PRESS,
    APP_KEY_EVENT_REPEAT,
} app_key_event_type_t;

typedef struct {
    app_key_event_type_t type;
    app_key_t key;
    int raw;
} app_key_event_t;

esp_err_t app_key_init(void);
app_key_t app_key_scan(void);
bool app_key_get_event(app_key_event_t *event);
int app_key_get_last_raw(void);
bool app_key_is_sw23_pressed(void);
bool app_key_is_sw23_long_pressed(uint32_t hold_ms);
const char *app_key_name(app_key_t key);
const char *app_key_event_name(app_key_event_type_t type);
