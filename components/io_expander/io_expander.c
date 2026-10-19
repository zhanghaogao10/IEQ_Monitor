#include "io_expander.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define IO_EXPANDER_I2C_PORT     I2C_NUM_0
#define IO_EXPANDER_I2C_FREQ_HZ  100000
#define IO_EXPANDER_I2C_SDA      GPIO_NUM_8
#define IO_EXPANDER_I2C_SCL      GPIO_NUM_9
#define IO_EXPANDER_ADDR         0x20
#define IO_EXPANDER_REG_INPUT    0x00
#define IO_EXPANDER_REG_OUTPUT   0x01
#define IO_EXPANDER_REG_POLARITY 0x02
#define IO_EXPANDER_REG_CONFIG   0x03

static const char *TAG = "io_expander";
static uint8_t s_output_state = 0xff;
static bool s_ready;
static bool s_driver_installed;

static esp_err_t io_expander_write_reg(uint8_t reg, uint8_t value)
{
    /* TPT29554A 兼容 9554 寄存器协议：先写寄存器地址，再写数据。 */
    uint8_t data[2] = {reg, value};
    return i2c_master_write_to_device(IO_EXPANDER_I2C_PORT, IO_EXPANDER_ADDR, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t io_expander_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_write_read_device(IO_EXPANDER_I2C_PORT, IO_EXPANDER_ADDR, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static esp_err_t io_expander_write_port(uint8_t value)
{
    return io_expander_write_reg(IO_EXPANDER_REG_OUTPUT, value);
}

static esp_err_t io_expander_read_port(uint8_t *value)
{
    return io_expander_read_reg(IO_EXPANDER_REG_INPUT, value);
}

static esp_err_t io_expander_probe_addr(void)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (IO_EXPANDER_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(IO_EXPANDER_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t io_expander_config_bus(void)
{
    if (s_driver_installed) {
        esp_err_t ret = i2c_driver_delete(IO_EXPANDER_I2C_PORT);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "delete previous I2C driver failed: %s", esp_err_to_name(ret));
        }
        s_driver_installed = false;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = IO_EXPANDER_I2C_SDA,
        .scl_io_num = IO_EXPANDER_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = IO_EXPANDER_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(IO_EXPANDER_I2C_PORT, &conf), TAG, "configure i2c failed");
    esp_err_t ret = i2c_driver_install(IO_EXPANDER_I2C_PORT, conf.mode, 0, 0, 0);
    if (ret == ESP_OK) {
        s_driver_installed = true;
    }
    return ret;
}

esp_err_t io_expander_init(void)
{
    /* 已确认本板地址 0x20，I2C 使用 GPIO8/GPIO9。 */
    if (s_ready) {
        return ESP_OK;
    }

    s_output_state = 0xff;
    ESP_LOGI(TAG, "init I2C bus SDA=%d SCL=%d addr=0x%02x", IO_EXPANDER_I2C_SDA, IO_EXPANDER_I2C_SCL, IO_EXPANDER_ADDR);
    esp_err_t ret = io_expander_config_bus();
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "idle level SDA=%d SCL=%d", gpio_get_level(IO_EXPANDER_I2C_SDA), gpio_get_level(IO_EXPANDER_I2C_SCL));

    ret = io_expander_probe_addr();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "io expander not found at 0x%02x", IO_EXPANDER_ADDR);
        return ret;
    }
    ESP_LOGI(TAG, "found io expander at 0x%02x", IO_EXPANDER_ADDR);

    ESP_RETURN_ON_ERROR(io_expander_write_reg(IO_EXPANDER_REG_POLARITY, 0x00), TAG, "set polarity failed");
    ESP_RETURN_ON_ERROR(io_expander_write_reg(IO_EXPANDER_REG_OUTPUT, s_output_state), TAG, "set output failed");
    /* 方向位：1 输入，0 输出。P0_7 KEY_CHECK 输入，P0_6 SW_IO 输出，P0_5 LCD_RESET 输出。 */
    ESP_RETURN_ON_ERROR(io_expander_write_reg(IO_EXPANDER_REG_CONFIG, 0x8f), TAG, "set config failed");

    ESP_LOGI(TAG, "io expander ready");
    s_ready = true;
    return ESP_OK;
}

bool io_expander_is_ready(void)
{
    return s_ready;
}

i2c_port_t io_expander_get_i2c_port(void)
{
    return IO_EXPANDER_I2C_PORT;
}

esp_err_t io_expander_set_level(uint8_t pin, bool level)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin > 7) {
        return ESP_ERR_INVALID_ARG;
    }

    if (level) {
        s_output_state |= BIT(pin);
    } else {
        s_output_state &= ~BIT(pin);
    }
    return io_expander_write_port(s_output_state);
}

esp_err_t io_expander_get_level(uint8_t pin, bool *level)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pin > 7 || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(io_expander_read_port(&value), TAG, "read port failed");
    *level = (value & BIT(pin)) != 0;
    return ESP_OK;
}
