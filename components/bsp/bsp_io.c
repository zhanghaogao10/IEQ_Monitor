#include "bsp.h"

#include "driver/ledc.h"

#define BSP_GPIO_LCD_BL GPIO_NUM_10
#define BSP_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_TIMER LEDC_TIMER_1
#define BSP_BL_LEDC_CHANNEL LEDC_CHANNEL_1
#define BSP_BL_FREQ_HZ 5000
#define BSP_BL_DUTY_RES LEDC_TIMER_10_BIT
#define BSP_BL_DUTY_MAX ((1U << 10) - 1)
#define BSP_BL_DEFAULT_BRIGHTNESS 80

static uint8_t s_backlight_brightness = BSP_BL_DEFAULT_BRIGHTNESS;
static bool s_backlight_ready;

esp_err_t bsp_io_init(void)
{
    /* IO10/LED_CTRL 使用 PWM 控制背光亮度。 */
    ledc_timer_config_t timer_conf = {
        .speed_mode = BSP_BL_LEDC_MODE,
        .duty_resolution = BSP_BL_DUTY_RES,
        .timer_num = BSP_BL_LEDC_TIMER,
        .freq_hz = BSP_BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t ret = ledc_timer_config(&timer_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    ledc_channel_config_t channel_conf = {
        .gpio_num = BSP_GPIO_LCD_BL,
        .speed_mode = BSP_BL_LEDC_MODE,
        .channel = BSP_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BSP_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ret = ledc_channel_config(&channel_conf);
    if (ret != ESP_OK) {
        return ret;
    }

    s_backlight_ready = true;
    return bsp_lcd_set_backlight_brightness(s_backlight_brightness);
}

void bsp_lcd_set_backlight(bool on)
{
    bsp_lcd_set_backlight_brightness(on ? s_backlight_brightness : 0);
}

esp_err_t bsp_lcd_set_backlight_brightness(uint8_t percent)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100) {
        percent = 100;
    }

    s_backlight_brightness = percent;
    uint32_t duty = (BSP_BL_DUTY_MAX * percent) / 100;
    esp_err_t ret = ledc_set_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL, duty);
    if (ret != ESP_OK) {
        return ret;
    }
    return ledc_update_duty(BSP_BL_LEDC_MODE, BSP_BL_LEDC_CHANNEL);
}

uint8_t bsp_lcd_get_backlight_brightness(void)
{
    return s_backlight_brightness;
}
