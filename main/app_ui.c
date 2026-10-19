#include "app_ui.h"

#include <inttypes.h>
#include <stdint.h>

#include "bsp.h"
#include "app_alarm.h"
#include "app_config.h"
#include "app_data.h"
#include "app_storage.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gui_guider.h"
#include "lvgl.h"

#define UI_LCD_WIDTH             360
#define UI_LCD_HEIGHT            360
#define UI_DRAW_BUF_LINES        180
#define UI_LVGL_TICK_PERIOD_MS   5
#define UI_BATTERY_CHARGE_STEP_MS 500

#if LV_COLOR_DEPTH != 16
#error "This LCD driver requires LV_COLOR_DEPTH=16/RGB565"
#endif

static const char *TAG = "app_ui";
static SemaphoreHandle_t s_ui_lock;
static lv_disp_t *s_display;
static lv_disp_draw_buf_t s_draw_buf_dsc;
static lv_disp_drv_t s_disp_drv;
static lv_color_t s_draw_buf[UI_LCD_WIDTH * UI_DRAW_BUF_LINES];
static bool s_showing_screen;
static bool s_settings_mode;
static uint8_t s_settings_index;
static bool s_stream_active;
static int s_stream_next_y;
static lv_obj_t *s_co_label;
static lv_obj_t *s_co2_label;
static lv_obj_t *s_temperature_label;
static lv_obj_t *s_humidity_label;
static lv_obj_t *s_co_raw_label;
static lv_obj_t *s_co_mv_label;
static lv_obj_t *s_threshold_name_label;
static lv_obj_t *s_threshold_value_label;
static int s_last_threshold_index = -1;
static float s_last_threshold_value;
static int s_last_co_ppm = -1;
static int s_last_co2_ppm = -1;
static int s_last_temperature_tenths = -10000;
static int s_last_humidity_rh = -1;
static int s_last_co_raw = -1;
static int s_last_co_mv = -1;
static int s_last_battery_display_percent = -1;
static int s_last_first_page_alert_mask = -1;
static bool s_last_battery_charging;
static bool s_battery_charging_initialized;
static int s_battery_charge_progress;
static int64_t s_battery_charge_last_step_us;
static lv_obj_t *s_last_screen_battery_bar;
static lv_obj_t *s_last_screen_1_battery_bar;
lv_ui guider_ui;

static void ui_load_screen(bool screen_1);

static void ui_load_screen_async(void *user_data)
{
    bool screen_1 = (bool)(uintptr_t)user_data;
    ESP_LOGI(TAG, "LVGL switch event execute: screen%s", screen_1 ? "_1" : "");
    ui_load_screen(screen_1);
}

static void ui_schedule_screen_switch(bool screen_1)
{
    /* Perform lv_scr_load from the LVGL timer context, not the key task. */
    lv_async_call(ui_load_screen_async, (void *)(uintptr_t)screen_1);
}

static void ui_save_thresholds_on_exit(void)
{
    app_alarm_thresholds_t thresholds;
    app_data_get_thresholds(&thresholds);
    esp_err_t ret = app_storage_save_thresholds(&thresholds);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "auto-save thresholds failed: %s", esp_err_to_name(ret));
    } else {
        app_alarm_thresholds_t saved;
        ret = app_storage_load_thresholds(&saved);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "saved alarm thresholds: temp=%.1f humi=%.0f co=%.0f co2=%.0f alarm=%umin",
                     saved.temp_high_c, saved.humidity_high_rh,
                     saved.co_high_ppm, saved.co2_high_ppm,
                     (unsigned)saved.alarm_duration_min);
        } else {
            ESP_LOGE(TAG, "read saved thresholds after auto-save failed: %s",
                     esp_err_to_name(ret));
        }
    }
}

static void ui_adjust_threshold(int direction)
{
    app_alarm_thresholds_t thresholds;
    app_data_get_thresholds(&thresholds);
    switch (s_settings_index) {
    case 0:
        thresholds.temp_high_c += direction * 0.5f;
        if (thresholds.temp_high_c < -20.0f) thresholds.temp_high_c = -20.0f;
        if (thresholds.temp_high_c > 80.0f) thresholds.temp_high_c = 80.0f;
        break;
    case 1:
        thresholds.humidity_high_rh += direction * 1.0f;
        if (thresholds.humidity_high_rh < 0.0f) thresholds.humidity_high_rh = 0.0f;
        if (thresholds.humidity_high_rh > 100.0f) thresholds.humidity_high_rh = 100.0f;
        break;
    case 2:
        thresholds.co_high_ppm += direction * 1.0f;
        if (thresholds.co_high_ppm < 0.0f) thresholds.co_high_ppm = 0.0f;
        if (thresholds.co_high_ppm > 500.0f) thresholds.co_high_ppm = 500.0f;
        break;
    case 3:
        thresholds.co2_high_ppm += direction * 50.0f;
        if (thresholds.co2_high_ppm < 400.0f) thresholds.co2_high_ppm = 400.0f;
        if (thresholds.co2_high_ppm > 5000.0f) thresholds.co2_high_ppm = 5000.0f;
        break;
    case 4: {
        int duration_min = (int)thresholds.alarm_duration_min + direction;
        if (duration_min < 1) duration_min = 1;
        if (duration_min > 10) duration_min = 10;
        thresholds.alarm_duration_min = (uint32_t)duration_min;
        break;
    }
    default:
        break;
    }
    app_data_set_thresholds(&thresholds);
#if APP_ENABLE_ALARM
    app_alarm_update();
#endif
}

static void ui_key_event_cb(lv_event_t *event)
{
    const uint32_t *key = lv_event_get_param(event);
    if (key == NULL) {
        return;
    }

    bool target_screen_1;
    switch (*key) {
    case LV_KEY_NEXT:
    case LV_KEY_RIGHT:
    case LV_KEY_ENTER:
        target_screen_1 = s_showing_screen;
        break;
    case LV_KEY_END:
        target_screen_1 = true;
        break;
    default:
        return;
    }

    ESP_LOGI(TAG, "LVGL key=%" PRIu32 ": switch to screen%s", *key,
             target_screen_1 ? "_1" : "");
    ui_schedule_screen_switch(target_screen_1);
}

static void ui_prepare_screen(lv_obj_t *screen)
{
    if (screen == NULL) {
        return;
    }

    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_size(screen, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

}

static bool ui_update_first_page_alert_colors(const app_sensor_data_t *sensor)
{
    if (sensor == NULL) {
        return false;
    }

    app_alarm_thresholds_t thresholds;
    app_data_get_thresholds(&thresholds);
    int alert_mask = 0;
    if (sensor->co_ppm >= thresholds.co_high_ppm) alert_mask |= 1 << 0;
    if (sensor->co2_ppm >= thresholds.co2_high_ppm) alert_mask |= 1 << 1;
    if (sensor->temperature_c >= thresholds.temp_high_c) alert_mask |= 1 << 2;
    if (sensor->humidity_rh >= thresholds.humidity_high_rh) alert_mask |= 1 << 3;

    bool changed = s_last_first_page_alert_mask != alert_mask;
    if (!changed) {
        return false;
    }

    lv_color_t yellow = lv_color_hex(0xFFD700);
    lv_color_t normal = lv_color_white();
    lv_obj_t *icons[] = {
        guider_ui.screen_img_1, /* CO */
        guider_ui.screen_img_2, /* CO2 */
        guider_ui.screen_img_3, /* temperature */
        guider_ui.screen_img_4, /* humidity */
    };
    lv_obj_t *data_labels[] = {
        guider_ui.screen_label_1, /* CO */
        guider_ui.screen_label_2, /* CO2 */
        guider_ui.screen_label_3, /* temperature */
        guider_ui.screen_label_4, /* humidity */
        guider_ui.screen_label_5, /* CO ADC */
        guider_ui.screen_label_6, /* CO mV */
    };
    bool icon_alert[] = {
        (alert_mask & (1 << 0)) != 0,
        (alert_mask & (1 << 1)) != 0,
        (alert_mask & (1 << 2)) != 0,
        (alert_mask & (1 << 3)) != 0,
    };
    bool label_alert[] = {
        icon_alert[0], icon_alert[1], icon_alert[2], icon_alert[3], false, false,
    };

    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        if (icons[i] != NULL) {
            lv_obj_set_style_img_recolor(icons[i], yellow,
                                         LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_img_recolor_opa(icons[i], icon_alert[i] ? 255 : 0,
                                             LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    for (size_t i = 0; i < sizeof(data_labels) / sizeof(data_labels[0]); i++) {
        if (data_labels[i] != NULL) {
            lv_obj_set_style_text_color(data_labels[i], label_alert[i] ? yellow : normal,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
    }
    s_last_first_page_alert_mask = alert_mask;
    return true;
}

static void ui_hide_first_page_adc_values(void)
{
    if (guider_ui.screen_label_5 != NULL) {
        lv_obj_add_flag(guider_ui.screen_label_5, LV_OBJ_FLAG_HIDDEN);
    }
    if (guider_ui.screen_label_6 != NULL) {
        lv_obj_add_flag(guider_ui.screen_label_6, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_force_refresh(void)
{
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(s_display);
}

static bool ui_update_sensor_labels(void)
{
    if (guider_ui.screen == NULL || lv_scr_act() != guider_ui.screen ||
        guider_ui.screen_label_1 == NULL ||
        guider_ui.screen_label_2 == NULL || guider_ui.screen_label_3 == NULL ||
        guider_ui.screen_label_4 == NULL || guider_ui.screen_label_5 == NULL ||
        guider_ui.screen_label_6 == NULL) {
        return false;
    }

    app_sensor_data_t sensor;
    app_data_get_sensor(&sensor);
    int co_ppm = sensor.co_ppm >= 0.0f
                     ? (int)(sensor.co_ppm + 0.5f)
                     : (int)(sensor.co_ppm - 0.5f);
    int co2_ppm = (int)(sensor.co2_ppm + 0.5f);
    int temperature_tenths = sensor.temperature_c >= 0.0f
                                  ? (int)(sensor.temperature_c * 10.0f + 0.5f)
                                  : (int)(sensor.temperature_c * 10.0f - 0.5f);
    int humidity_rh = (int)(sensor.humidity_rh + 0.5f);
    int co_raw = sensor.co_raw;
    int co_mv = sensor.co_mv;
    bool changed = ui_update_first_page_alert_colors(&sensor);

    if (s_co_label != guider_ui.screen_label_1 || s_last_co_ppm != co_ppm) {
        lv_label_set_text_fmt(guider_ui.screen_label_1, "%d ppm", co_ppm);
        s_co_label = guider_ui.screen_label_1;
        s_last_co_ppm = co_ppm;
        changed = true;
    }

    if (s_co2_label != guider_ui.screen_label_2 || s_last_co2_ppm != co2_ppm) {
        lv_label_set_text_fmt(guider_ui.screen_label_2, "%d ppm", co2_ppm);
        s_co2_label = guider_ui.screen_label_2;
        s_last_co2_ppm = co2_ppm;
        changed = true;
    }

    if (s_temperature_label != guider_ui.screen_label_3 ||
        s_last_temperature_tenths != temperature_tenths) {
        int temperature_abs = temperature_tenths < 0 ? -temperature_tenths : temperature_tenths;
        lv_label_set_text_fmt(guider_ui.screen_label_3, "%s%d.%d℃",
                              temperature_tenths < 0 ? "-" : "",
                              temperature_abs / 10, temperature_abs % 10);
        s_temperature_label = guider_ui.screen_label_3;
        s_last_temperature_tenths = temperature_tenths;
        changed = true;
    }

    if (s_humidity_label != guider_ui.screen_label_4 || s_last_humidity_rh != humidity_rh) {
        lv_label_set_text_fmt(guider_ui.screen_label_4, "%d%%RH", humidity_rh);
        s_humidity_label = guider_ui.screen_label_4;
        s_last_humidity_rh = humidity_rh;
        changed = true;
    }

    if (s_co_raw_label != guider_ui.screen_label_5 || s_last_co_raw != co_raw) {
        lv_label_set_text_fmt(guider_ui.screen_label_5, "ADC: %d", co_raw);
        s_co_raw_label = guider_ui.screen_label_5;
        s_last_co_raw = co_raw;
        changed = true;
    }

    if (s_co_mv_label != guider_ui.screen_label_6 || s_last_co_mv != co_mv) {
        lv_label_set_text_fmt(guider_ui.screen_label_6, "%dmV", co_mv);
        s_co_mv_label = guider_ui.screen_label_6;
        s_last_co_mv = co_mv;
        changed = true;
    }

    return changed;
}

static void ui_update_battery_bar_value(lv_obj_t *bar, int battery_percent)
{
    if (bar == NULL) {
        return;
    }

    if (battery_percent < 0) battery_percent = 0;
    if (battery_percent > 100) battery_percent = 100;
    lv_bar_set_value(bar, battery_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_white(),
                              LV_PART_INDICATOR | LV_STATE_DEFAULT);
}

static void ui_update_charging_icons(bool charging)
{
    if (guider_ui.screen != NULL && !guider_ui.screen_del &&
        guider_ui.screen_img_5 != NULL) {
        if (charging) {
            lv_obj_clear_flag(guider_ui.screen_img_5, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(guider_ui.screen_img_5, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (guider_ui.screen_1 != NULL && !guider_ui.screen_1_del &&
        guider_ui.screen_1_img_2 != NULL) {
        if (charging) {
            lv_obj_clear_flag(guider_ui.screen_1_img_2, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(guider_ui.screen_1_img_2, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static bool ui_update_battery_bars(void)
{
    app_sensor_data_t sensor;
    app_data_get_sensor(&sensor);
    int64_t now_us = esp_timer_get_time();
    int display_percent = sensor.battery_percent;
    bool charge_progress_changed = false;

    if (sensor.battery_full) {
        /* 只要检测到 full=1，就固定显示 100%，停止充电动画。 */
        s_battery_charging_initialized = true;
        s_battery_charge_progress = 100;
        display_percent = 100;
    } else if (sensor.battery_charging) {
        if (!s_battery_charging_initialized || !s_last_battery_charging) {
            /* 进入充电状态时从 0% 开始，每次增加 20%。 */
            s_battery_charge_progress = 0;
            s_battery_charge_last_step_us = now_us;
            s_battery_charging_initialized = true;
            charge_progress_changed = true;
        } else if (now_us - s_battery_charge_last_step_us >=
                   (int64_t)UI_BATTERY_CHARGE_STEP_MS * 1000LL) {
            s_battery_charge_progress += 20;
            if (s_battery_charge_progress > 100) {
                s_battery_charge_progress = 0;
            }
            s_battery_charge_last_step_us = now_us;
            charge_progress_changed = true;
        }
        display_percent = s_battery_charge_progress;
    }

    bool screen_bar_changed = s_last_screen_battery_bar != guider_ui.screen_bar_1;
    bool screen_1_bar_changed = s_last_screen_1_battery_bar != guider_ui.screen_1_bar_1;
    bool changed = screen_bar_changed || screen_1_bar_changed ||
                   s_last_battery_display_percent != display_percent ||
                   s_last_battery_charging != sensor.battery_charging ||
                   charge_progress_changed;

    if (!changed) {
        return false;
    }

    ui_update_charging_icons(sensor.battery_charging);

    if (guider_ui.screen != NULL && !guider_ui.screen_del &&
        guider_ui.screen_bar_1 != NULL) {
        ui_update_battery_bar_value(guider_ui.screen_bar_1, display_percent);
    }
    if (guider_ui.screen_1 != NULL && !guider_ui.screen_1_del &&
        guider_ui.screen_1_bar_1 != NULL) {
        ui_update_battery_bar_value(guider_ui.screen_1_bar_1, display_percent);
    }
    s_last_battery_display_percent = display_percent;
    s_last_battery_charging = sensor.battery_charging;
    s_last_screen_battery_bar = guider_ui.screen_bar_1;
    s_last_screen_1_battery_bar = guider_ui.screen_1_bar_1;
    return changed;
}

static bool ui_update_threshold_labels(void)
{
    if (!s_settings_mode || guider_ui.screen_1 == NULL ||
        lv_scr_act() != guider_ui.screen_1 ||
        guider_ui.screen_1_label_2 == NULL || guider_ui.screen_1_label_3 == NULL) {
        return false;
    }

    app_alarm_thresholds_t thresholds;
    app_data_get_thresholds(&thresholds);
    float value = 0.0f;
    const char *name = "TEMP THRESHOLD";

    switch (s_settings_index) {
    case 0:
        name = "TEMP THRESHOLD";
        value = thresholds.temp_high_c;
        break;
    case 1:
        name = "HUMI THRESHOLD";
        value = thresholds.humidity_high_rh;
        break;
    case 2:
        name = "CO THRESHOLD";
        value = thresholds.co_high_ppm;
        break;
    case 3:
        name = "CO2 THRESHOLD";
        value = thresholds.co2_high_ppm;
        break;
    case 4:
        name = "ALARM TIME";
        value = (float)thresholds.alarm_duration_min;
        break;
    default:
        return false;
    }

    if (s_threshold_name_label == guider_ui.screen_1_label_2 &&
        s_threshold_value_label == guider_ui.screen_1_label_3 &&
        s_last_threshold_index == s_settings_index && s_last_threshold_value == value) {
        return false;
    }
    lv_label_set_text(guider_ui.screen_1_label_2, name);
    if (s_settings_index == 0) {
        int temp_tenths = value >= 0.0f ? (int)(value * 10.0f + 0.5f)
                                       : (int)(value * 10.0f - 0.5f);
        int temp_abs = temp_tenths < 0 ? -temp_tenths : temp_tenths;
        lv_label_set_text_fmt(guider_ui.screen_1_label_3, "< %s%d.%d℃ >",
                              temp_tenths < 0 ? "-" : "", temp_abs / 10, temp_abs % 10);
    } else if (s_settings_index == 1) {
        lv_label_set_text_fmt(guider_ui.screen_1_label_3, "< %d%%RH >", (int)(value + 0.5f));
    } else if (s_settings_index == 4) {
        lv_label_set_text_fmt(guider_ui.screen_1_label_3, "< %dmin >", (int)(value + 0.5f));
    } else {
        lv_label_set_text_fmt(guider_ui.screen_1_label_3, "< %dppm >", (int)(value + 0.5f));
    }
    s_threshold_name_label = guider_ui.screen_1_label_2;
    s_threshold_value_label = guider_ui.screen_1_label_3;
    s_last_threshold_index = s_settings_index;
    s_last_threshold_value = value;
    return true;
}

static void ui_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(UI_LVGL_TICK_PERIOD_MS);
}

static void ui_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    int width = area->x2 - area->x1 + 1;
    int height = area->y2 - area->y1 + 1;
    bool is_full_width = area->x1 == 0 && area->x2 == UI_LCD_WIDTH - 1;
    bool is_last = lv_disp_flush_is_last(disp_drv);

    if (width <= 0 || height <= 0 || color_p == NULL) {
        ESP_LOGE(TAG, "invalid flush area x=%d..%d y=%d..%d", area->x1, area->x2, area->y1, area->y2);
        lv_disp_flush_ready(disp_drv);
        return;
    }

    if (!is_full_width || (!s_stream_active && area->y1 != 0) ||
        (s_stream_active && area->y1 != s_stream_next_y)) {
        ESP_LOGE(TAG, "non-continuous frame area x=%d..%d y=%d..%d", area->x1, area->x2,
                 area->y1, area->y2);
        if (s_stream_active) {
            bsp_lcd_stream_end();
            s_stream_active = false;
        }
        lv_disp_flush_ready(disp_drv);
        return;
    }

    esp_err_t ret = ESP_OK;
    if (!s_stream_active) {
        ret = bsp_lcd_stream_start();
        s_stream_active = ret == ESP_OK;
        s_stream_next_y = 0;
    }

    if (ret == ESP_OK) {
        size_t stride = (size_t)width * sizeof(lv_color_t);
        ret = bsp_lcd_stream_write_bitmap(width, height, color_p, stride);
        if (ret == ESP_OK) {
            s_stream_next_y = area->y2 + 1;
        }
    }

    if (is_last || area->y2 == UI_LCD_HEIGHT - 1) {
        if (s_stream_next_y != UI_LCD_HEIGHT) {
            ESP_LOGE(TAG, "incomplete frame: final y=%d", s_stream_next_y);
        }
        if (s_stream_active) {
            bsp_lcd_stream_end();
            s_stream_active = false;
        }
        s_stream_next_y = 0;
    } else if (ret != ESP_OK && s_stream_active) {
        bsp_lcd_stream_end();
        s_stream_active = false;
        s_stream_next_y = 0;
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(ret));
    }
    lv_disp_flush_ready(disp_drv);
}

esp_err_t app_ui_init(void)
{
    static StaticSemaphore_t lock_buffer;
    s_ui_lock = xSemaphoreCreateMutexStatic(&lock_buffer);
    if (s_ui_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_draw_buf, NULL, UI_LCD_WIDTH * UI_DRAW_BUF_LINES);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = UI_LCD_WIDTH;
    s_disp_drv.ver_res = UI_LCD_HEIGHT;
    s_disp_drv.flush_cb = ui_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    /* 360x180 is not a full frame buffer, so full_refresh must remain disabled. */
    s_disp_drv.full_refresh = 0;
    s_display = lv_disp_drv_register(&s_disp_drv);
    if (s_display == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_add_event_cb(lv_layer_top(), ui_key_event_cb, LV_EVENT_KEY, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = ui_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &timer), TAG, "create LVGL tick failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(timer, UI_LVGL_TICK_PERIOD_MS * 1000), TAG,
                        "start LVGL tick failed");

    setup_ui(&guider_ui);
    ui_prepare_screen(guider_ui.screen);
    ui_hide_first_page_adc_values();
    setup_scr_screen_1(&guider_ui);
    ui_prepare_screen(guider_ui.screen_1);
    lv_scr_load(guider_ui.screen);
    guider_ui.screen_del = false;
    guider_ui.screen_1_del = false;
    s_showing_screen = true;
    s_settings_mode = false;
    ui_force_refresh();

    ESP_LOGI(TAG, "GUI Guider UI ready");
    return ESP_OK;
}

static lv_obj_t *ui_get_screen(bool screen_1)
{
    lv_obj_t **screen = screen_1 ? &guider_ui.screen_1 : &guider_ui.screen;
    bool *screen_del = screen_1 ? &guider_ui.screen_1_del : &guider_ui.screen_del;

    if (*screen == NULL || *screen_del) {
        if (screen_1) {
            setup_scr_screen_1(&guider_ui);
        } else {
            setup_scr_screen(&guider_ui);
        }
        *screen_del = false;
    }

    ui_prepare_screen(*screen);
    if (!screen_1) {
        ui_hide_first_page_adc_values();
    }
    return *screen;
}

static void ui_delete_screen(lv_obj_t *screen)
{
    if (screen == guider_ui.screen) {
        lv_obj_del(screen);
        guider_ui.screen = NULL;
        guider_ui.screen_del = true;
        guider_ui.screen_bar_1 = NULL;
        s_last_screen_battery_bar = NULL;
        s_co_label = NULL;
        s_co2_label = NULL;
        s_temperature_label = NULL;
        s_humidity_label = NULL;
        s_co_raw_label = NULL;
        s_co_mv_label = NULL;
        s_last_first_page_alert_mask = -1;
    } else if (screen == guider_ui.screen_1) {
        lv_obj_del(screen);
        guider_ui.screen_1 = NULL;
        guider_ui.screen_1_del = true;
        guider_ui.screen_1_bar_1 = NULL;
        s_last_screen_1_battery_bar = NULL;
        s_threshold_name_label = NULL;
        s_threshold_value_label = NULL;
        s_last_threshold_index = -1;
    }
}

static void ui_load_screen(bool screen_1)
{
    lv_obj_t *screen = ui_get_screen(screen_1);
    if (screen == NULL) {
        ESP_LOGE(TAG, "target screen is not ready");
        return;
    }

    lv_obj_t *old_screen = lv_scr_act();
    lv_scr_load(screen);
    s_showing_screen = !screen_1;
    ESP_LOGI(TAG, "load screen%s", screen_1 ? "_1" : "");
    ui_update_battery_bars();
    ui_update_threshold_labels();
    ui_force_refresh();

    if (old_screen != screen) {
        ui_delete_screen(old_screen);
    }
}

void app_ui_update(void)
{
    if (s_display == NULL || xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    if (ui_update_sensor_labels() || ui_update_battery_bars() || ui_update_threshold_labels()) {
        /* The LCD stream backend only accepts a full-width, full-frame refresh. */
        ui_force_refresh();
    } else {
        lv_timer_handler();
    }
    xSemaphoreGive(s_ui_lock);
}

void app_ui_handle_key_event(const app_key_event_t *event)
{
    if (event == NULL || event->type == APP_KEY_EVENT_NONE) {
        return;
    }

    if (event->type != APP_KEY_EVENT_SHORT_PRESS &&
        event->type != APP_KEY_EVENT_LONG_PRESS &&
        event->type != APP_KEY_EVENT_REPEAT) {
        return;
    }

    if (s_display == NULL || xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "drop key event: ui busy");
        return;
    }

    /* SW1 长按在普通页面和设置页面之间切换。 */
    if (event->key == APP_KEY_SW1 && event->type == APP_KEY_EVENT_LONG_PRESS) {
        if (s_settings_mode) {
            ui_save_thresholds_on_exit();
        }
        s_settings_mode = !s_settings_mode;
        s_settings_index = 0;
        ui_schedule_screen_switch(s_settings_mode);
        xSemaphoreGive(s_ui_lock);
        return;
    }

    bool setting_short_press = event->type == APP_KEY_EVENT_SHORT_PRESS;
    bool setting_repeat = event->type == APP_KEY_EVENT_REPEAT;
    if (s_settings_mode && (setting_short_press || setting_repeat)) {
        if (event->key == APP_KEY_SW1) {
            /* SW1 只在短按时切换参数，长按后的重复事件不切换。 */
            if (setting_short_press) {
                s_settings_index = (s_settings_index + 1) % 5;
            }
        } else if (event->key == APP_KEY_SW2) {
            /* SW2 增加当前参数；短按一次，长按后按重复事件继续增加。 */
            ui_adjust_threshold(1);
        } else if (event->key == APP_KEY_SW3) {
            /* SW3 减小当前参数；短按一次，长按后按重复事件继续减小。 */
            ui_adjust_threshold(-1);
        }
        xSemaphoreGive(s_ui_lock);
        return;
    }

    /* 普通数据显示页面不响应短按，只有 SW1 长按可进入设置页面。 */
    if (!s_settings_mode) {
        xSemaphoreGive(s_ui_lock);
        return;
    }

    /* 设置页面暂未定义其他按键/长按行为。 */
    xSemaphoreGive(s_ui_lock);
}

void app_ui_prepare_shutdown(void)
{
    if (s_ui_lock == NULL || xSemaphoreTake(s_ui_lock, pdMS_TO_TICKS(250)) != pdTRUE) {
        ESP_LOGW(TAG, "shutdown requested while UI is busy");
        return;
    }

    /* 如果正在设置页面，关机前先保存当前阈值。 */
    if (s_settings_mode) {
        ui_save_thresholds_on_exit();
        s_settings_mode = false;
    }

    xSemaphoreGive(s_ui_lock);
}
