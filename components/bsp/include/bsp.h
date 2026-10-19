#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* BSP：板级 LCD、背光和基础 IO 封装。 */
esp_err_t bsp_io_init(void);
esp_err_t bsp_lcd_init(void);
void bsp_lcd_set_backlight(bool on);
esp_err_t bsp_lcd_set_backlight_brightness(uint8_t percent);
uint8_t bsp_lcd_get_backlight_brightness(void);
esp_err_t bsp_lcd_stream_start(void);
esp_err_t bsp_lcd_stream_write_bitmap(int width, int height, const void *color_data, size_t stride_bytes);
void bsp_lcd_stream_end(void);
