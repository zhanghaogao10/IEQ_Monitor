#include "app_power.h"

#include "app_config.h"
#include "esp_log.h"
#include "io_expander.h"

static const char *TAG = "app_power";
static bool s_key_pressed;

esp_err_t app_power_init(void)
{
    /* 上电后尽快拉住 SW_IO，避免松开 SW1 后电源掉电。 */
    if (!io_expander_is_ready()) {
        ESP_LOGW(TAG, "io expander is not ready, power hold control disabled");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = app_power_set_hold(true);
    if (ret != ESP_OK) {
        return ret;
    }

    bool level = false;
    ret = io_expander_get_level(APP_IO_EXPANDER_PIN_KEY_CHECK, &level);
    if (ret == ESP_OK) {
        s_key_pressed = level;
        ESP_LOGI(TAG, "power key=%s", s_key_pressed ? "pressed" : "released");
    }
    return ret;
}

esp_err_t app_power_set_hold(bool on)
{
    ESP_LOGI(TAG, "power hold %s", on ? "on" : "off");
    return io_expander_set_level(APP_IO_EXPANDER_PIN_SW_IO, on);
}

esp_err_t app_power_request_shutdown(void)
{
    ESP_LOGI(TAG, "request shutdown");
    return app_power_set_hold(false);
}

bool app_power_is_key_pressed(void)
{
    /* KEY_CHECK 高电平表示 SW1 按下。 */
    if (!io_expander_is_ready()) {
        return false;
    }

    bool level = true;
    if (io_expander_get_level(APP_IO_EXPANDER_PIN_KEY_CHECK, &level) != ESP_OK) {
        return s_key_pressed;
    }

    s_key_pressed = level;
    return s_key_pressed;
}
