#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c.h"
#include "esp_err.h"

/* TPT29554A：I2C 转 8 位 GPIO 扩展器。 */
esp_err_t io_expander_init(void);
bool io_expander_is_ready(void);
i2c_port_t io_expander_get_i2c_port(void);
esp_err_t io_expander_set_level(uint8_t pin, bool level);
esp_err_t io_expander_get_level(uint8_t pin, bool *level);
