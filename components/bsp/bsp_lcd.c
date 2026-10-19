#include "bsp.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io_expander.h"

#define LCD_WIDTH                360
#define LCD_HEIGHT               360
/* Signal names match the board schematic. */
#define LCD_CS                   GPIO_NUM_5
#define LCD_SDA                  GPIO_NUM_6
#define LCD_SCL                  GPIO_NUM_7
#define LCD_RS                   GPIO_NUM_2
#define LCD_RESET                5
#define LCD_SPI_HOST             SPI2_HOST
#define LCD_SPI_CLOCK_HZ         (60 * 1000 * 1000)
#define LCD_PIXEL_BYTES          sizeof(uint16_t)
#define LCD_SPI_MAX_TRANSFER     (LCD_WIDTH * LCD_PIXEL_BYTES)
#define LCD_SPI_FIFO_MAX_TRANSFER 64
#define LCD_SIGNAL_HOLD_US       2
static const char *TAG = "bsp_lcd";
static spi_device_handle_t s_lcd_spi;
static bool s_lcd_streaming;
static bool s_lcd_memory_write_active;
static size_t s_lcd_expected_pixel_bytes;
static size_t s_lcd_sent_pixel_bytes;

/* 厂家资料与测试工程均使用 360x360 有效显示区域。 */
typedef struct {
    uint8_t cmd;
    const uint8_t *data;
    uint8_t data_len;
    uint16_t delay_ms;
} lcd_init_cmd_t;

#define LCD_CMD(_cmd, ...) { .cmd = (_cmd), .data = (const uint8_t[]){__VA_ARGS__}, .data_len = sizeof((const uint8_t[]){__VA_ARGS__}) }
#define LCD_DELAY(_ms)    { .cmd = 0x00, .data = NULL, .data_len = 0, .delay_ms = (_ms) }
#define LCD_CMD0(_cmd)    { .cmd = (_cmd), .data = NULL, .data_len = 0 }

static esp_err_t lcd_spi_write(const void *data, size_t len)
{
    if (len == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_lcd_spi == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t *bytes = data;
    while (len > 0) {
        /* 无 DMA 时 SPI 硬件 FIFO 单次最多传输 64 字节。 */
        size_t chunk_len = len > LCD_SPI_FIFO_MAX_TRANSFER ? LCD_SPI_FIFO_MAX_TRANSFER : len;
        spi_transaction_t trans = {
            .length = chunk_len * 8,
            .tx_buffer = bytes,
        };
        esp_err_t ret = spi_device_polling_transmit(s_lcd_spi, &trans);
        if (ret != ESP_OK) {
            return ret;
        }
        bytes += chunk_len;
        len -= chunk_len;
    }
    return ESP_OK;
}

static esp_err_t lcd_write_cmd(uint8_t cmd)
{
    /* Set the bus mode before selecting the LCD. */
    gpio_set_level(LCD_RS, 0);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_CS, 0);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    esp_err_t ret = lcd_spi_write(&cmd, sizeof(cmd));
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_CS, 1);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    return ret;
}

static esp_err_t lcd_write_data(const void *data, size_t len)
{
    if (len == 0 || data == NULL) {
        return ESP_OK;
    }

    /* Set data mode before selecting the LCD. */
    gpio_set_level(LCD_RS, 1);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_CS, 0);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);

    esp_err_t ret = lcd_spi_write(data, len);

    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_CS, 1);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    return ret;
}

/* Keep RAMWR and its pixel stream inside one CS assertion. */
static esp_err_t lcd_begin_memory_write(void)
{
    const uint8_t cmd = 0x2C;

    if (s_lcd_expected_pixel_bytes == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    gpio_set_level(LCD_RS, 0);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_CS, 0);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);

    esp_err_t ret = lcd_spi_write(&cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        gpio_set_level(LCD_CS, 1);
        return ret;
    }

    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    gpio_set_level(LCD_RS, 1);
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);
    s_lcd_sent_pixel_bytes = 0;
    s_lcd_memory_write_active = true;
    return ESP_OK;
}

static esp_err_t lcd_end_data(void)
{
    esp_err_t ret = ESP_OK;

    if (s_lcd_memory_write_active &&
        s_lcd_sent_pixel_bytes != s_lcd_expected_pixel_bytes) {

        ESP_LOGE(TAG,
                 "pixel length mismatch: sent=%u expected=%u",
                 (unsigned)s_lcd_sent_pixel_bytes,
                 (unsigned)s_lcd_expected_pixel_bytes);
        ret = ESP_ERR_INVALID_SIZE;
    }

    s_lcd_memory_write_active = false;

    /* 最后一个 SPI bit 已经由 polling_transmit 等待完成 */
    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);

    /* 立即结束 RAMWR transaction */
    gpio_set_level(LCD_CS, 1);

    esp_rom_delay_us(LCD_SIGNAL_HOLD_US);

    return ret;
}

static esp_err_t lcd_send_bytes(const void *data, size_t len)
{
    if (!s_lcd_memory_write_active) {
        ESP_LOGE(TAG, "pixel data sent without active RAMWR transaction");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = lcd_spi_write(data, len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi data write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_lcd_sent_pixel_bytes += len;
    return ESP_OK;
}

static esp_err_t lcd_set_window_address(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    ESP_RETURN_ON_ERROR(lcd_write_cmd(0x2A), TAG, "write CASET failed");
    data[0] = x0 >> 8;
    data[1] = x0 & 0xff;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xff;
    ESP_RETURN_ON_ERROR(lcd_write_data(data, sizeof(data)), TAG, "write CASET data failed");

    ESP_RETURN_ON_ERROR(lcd_write_cmd(0x2B), TAG, "write RASET failed");
    data[0] = y0 >> 8;
    data[1] = y0 & 0xff;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xff;
    ESP_RETURN_ON_ERROR(lcd_write_data(data, sizeof(data)), TAG, "write RASET data failed");

    return ESP_OK;
}

static esp_err_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    if (x1 < x0 || y1 < y0 || x1 >= LCD_WIDTH || y1 >= LCD_HEIGHT) {
        ESP_LOGE(TAG, "window out of range: %u..%u, %u..%u",
                 (unsigned)x0, (unsigned)x1, (unsigned)y0, (unsigned)y1);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = lcd_set_window_address(x0, y0, x1, y1);
    if (ret == ESP_OK) {
        s_lcd_expected_pixel_bytes = (size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1) * LCD_PIXEL_BYTES;
        s_lcd_sent_pixel_bytes = 0;
        s_lcd_memory_write_active = false;
    }
    return ret;
}

static esp_err_t lcd_apply_init_sequence(void)
{
    /* 初始化序列来自厂家 lcd.c 的 LCD_Init()。 */
    const lcd_init_cmd_t init_cmds[] = {
        LCD_CMD0(0xFE), LCD_CMD0(0xEF),
        LCD_CMD(0x80, 0x19), LCD_CMD(0x82, 0x09), LCD_CMD(0x83, 0x03),
        LCD_CMD(0x88, 0x00), LCD_CMD(0x89, 0x38), LCD_CMD(0x8A, 0x40),
        LCD_CMD(0x8B, 0x0A), LCD_CMD(0x8C, 0x00),
        LCD_CMD(0x81, 0xFF), LCD_CMD(0x84, 0xFF), LCD_CMD(0x85, 0xFF),
        LCD_CMD(0x86, 0xFF), LCD_CMD(0x87, 0xFF), LCD_CMD(0x8E, 0xFF),
        LCD_CMD(0x8F, 0xFF), LCD_CMD(0x98, 0x3E), LCD_CMD(0x99, 0x3E),
        LCD_CMD(0x7D, 0x72),
        LCD_CMD(0x70, 0x02, 0x03, 0x03, 0x06, 0x03, 0x03, 0x09, 0x07, 0x09, 0x03),
        LCD_CMD(0x90, 0x06, 0x06, 0x01, 0x01),
        LCD_CMD(0x93, 0x02, 0xFF, 0x00), LCD_CMD(0xCB, 0x02),
        LCD_CMD(0xFB, 0x00, 0x00), LCD_CMD(0xF6, 0xC0),
        LCD_CMD(0x6C, 0x00, 0x00, 0x22, 0x00, 0xCC, 0x04, 0x58),
        LCD_CMD(0xAA, 0x0B, 0x00), LCD_CMD(0xEC, 0x07), LCD_CMD(0xF9, 0x40),
        LCD_CMD(0xEB, 0x01, 0x67),
        LCD_CMD(0x74, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00),
        LCD_CMD(0xB5, 0x14, 0x14, 0x14),
        LCD_CMD(0x6E, 0x0B, 0x0B, 0x09, 0x09, 0x13, 0x13, 0x11, 0x11,
                0x16, 0x15, 0x01, 0x04, 0x00, 0x0D, 0x1D, 0x00, 0x00,
                0x1D, 0x0D, 0x00, 0x04, 0x08, 0x15, 0x16, 0x12, 0x12,
                0x14, 0x14, 0x0A, 0x0A, 0x0C, 0x0C),
        LCD_CMD(0x60, 0x38, 0x1C, 0x13, 0x56), LCD_CMD(0x61, 0xF8, 0x0A, 0x13, 0x56),
        LCD_CMD(0x62, 0xF8, 0x0B, 0x13, 0x56), LCD_CMD(0x63, 0x38, 0x1C, 0x13, 0x56),
        LCD_CMD(0x64, 0x38, 0x20, 0x72, 0xF8, 0x13, 0x56),
        LCD_CMD(0x65, 0x78, 0x1A, 0x70, 0x0B, 0x56, 0x13),
        LCD_CMD(0x66, 0x38, 0x24, 0x72, 0xFC, 0x13, 0x56),
        LCD_CMD(0x68, 0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A),
        LCD_CMD(0x69, 0xB3, 0x08, 0x0E, 0x08, 0x0E, 0x0A, 0x0A),
        LCD_CMD(0x6A, 0x00, 0x00), LCD_CMD(0x3A, 0x05),
        /* RGB color order; keep the vendor axis orientation unchanged. */
        LCD_CMD(0x36, 0x00),
        LCD_CMD(0x7C, 0xB6, 0x29), LCD_CMD(0xAC, 0x40), LCD_CMD(0xC3, 0x1A),
        LCD_CMD(0xC4, 0x24), LCD_CMD(0xC9, 0x2F),
        LCD_CMD(0xF0, 0x11, 0x17, 0x08, 0x06, 0x05, 0x38),
        LCD_CMD(0xF1, 0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F),
        LCD_CMD(0xF2, 0x11, 0x17, 0x08, 0x06, 0x05, 0x38),
        LCD_CMD(0xF3, 0x4D, 0x72, 0x72, 0x2D, 0x34, 0x8F),
        /* TE is not wired to the ESP32-C3, so keep its output disabled. */
        LCD_CMD(0xB4, 0x0A), LCD_CMD0(0x34), LCD_CMD0(0xFE), LCD_CMD0(0xEE),
        /* The controller needs a full wake-up interval after Sleep Out. */
        LCD_CMD0(0x11), LCD_DELAY(150), LCD_CMD0(0x29), LCD_DELAY(20),
    };

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        if (init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
            continue;
        }
        ESP_RETURN_ON_ERROR(lcd_write_cmd(init_cmds[i].cmd), TAG, "write init cmd failed");
        ESP_RETURN_ON_ERROR(lcd_write_data(init_cmds[i].data, init_cmds[i].data_len), TAG, "write init data failed");
    }
    return ESP_OK;
}

esp_err_t bsp_lcd_init(void)
{
    /* LCD_RESET 通过 TPT29554A 的 P0_5 控制。 */
    ESP_LOGI(TAG, "init lcd spi cpu fifo CS=%d SDA=%d SCL=%d RS=%d RESET=P0_%d size=%dx%d clock=%d",
             LCD_CS, LCD_SDA, LCD_SCL, LCD_RS, LCD_RESET,
             LCD_WIDTH, LCD_HEIGHT, LCD_SPI_CLOCK_HZ);

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LCD_RS) | (1ULL << LCD_CS) |
                        (1ULL << LCD_SDA) | (1ULL << LCD_SCL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "configure lcd gpio failed");
    gpio_set_level(LCD_CS, 1);
    gpio_set_level(LCD_SCL, 1);

    if (s_lcd_spi == NULL) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = LCD_SDA,
            .miso_io_num = -1,
            .sclk_io_num = LCD_SCL,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = LCD_SPI_MAX_TRANSFER,
        };
        spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = LCD_SPI_CLOCK_HZ,
            .mode = 0,
            .spics_io_num = -1,
            .queue_size = 1,
            .cs_ena_pretrans = 1,
            .cs_ena_posttrans = 1,
        };

        ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_DISABLED),
                            TAG, "initialize lcd spi bus failed");
        ESP_RETURN_ON_ERROR(spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_lcd_spi),
                            TAG, "add lcd spi device failed");
    }

    if (io_expander_is_ready()) {
        ESP_LOGI(TAG, "toggle lcd reset via io expander P0_%d", LCD_RESET);
        ESP_RETURN_ON_ERROR(io_expander_set_level(LCD_RESET, true), TAG, "lcd reset high failed");
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_RETURN_ON_ERROR(io_expander_set_level(LCD_RESET, false), TAG, "lcd reset low failed");
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_RETURN_ON_ERROR(io_expander_set_level(LCD_RESET, true), TAG, "lcd reset release failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_LOGW(TAG, "skip lcd hardware reset because io expander is not ready");
    }

    ESP_RETURN_ON_ERROR(lcd_apply_init_sequence(), TAG, "lcd init sequence failed");
    ESP_LOGI(TAG, "lcd init sequence done");
    return ESP_OK;
}

esp_err_t bsp_lcd_stream_start(void)
{
    if (s_lcd_streaming) {
        /* A previous LVGL frame was abandoned before its logical end. */
        (void)lcd_end_data();
        s_lcd_streaming = false;
    }

    if (!s_lcd_memory_write_active) {
        /* First LVGL frame: configure one full-screen window and enter RAMWR. */
        ESP_RETURN_ON_ERROR(lcd_set_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1), TAG,
                            "set stream full window failed");
        ESP_RETURN_ON_ERROR(lcd_begin_memory_write(), TAG, "start stream memory write failed");
        ESP_LOGI(TAG, "LVGL stream first frame: CASET/RASET/RAMWR, keep CS low");
    } else {
        /* Later frames: keep CS low and RS high; emit pixel bytes only. */
        s_lcd_sent_pixel_bytes = 0;
    }

    s_lcd_streaming = true;
    return ESP_OK;
}

esp_err_t bsp_lcd_stream_write_bitmap(int width, int height, const void *color_data, size_t stride_bytes)
{
    if (!s_lcd_streaming || color_data == NULL || width <= 0 || height <= 0 ||
        width > LCD_WIDTH || height > LCD_HEIGHT || stride_bytes < (size_t)width * sizeof(uint16_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *src = (const uint8_t *)color_data;
    static uint16_t tx_line[LCD_WIDTH];

    for (int y = 0; y < height; y++) {
        const uint16_t *src_line = (const uint16_t *)(src + (size_t)y * stride_bytes);
        for (int x = 0; x < width; x++) {
            /* LV_COLOR_16_SWAP gives the SPI byte order required by this LCD. */
            tx_line[x] = src_line[x];
        }
        esp_err_t ret = lcd_send_bytes(tx_line, (size_t)width * sizeof(uint16_t));
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

void bsp_lcd_stream_end(void)
{
    if (s_lcd_streaming) {
        if (s_lcd_sent_pixel_bytes != s_lcd_expected_pixel_bytes) {
            ESP_LOGE(TAG, "LVGL stream frame length mismatch: sent=%u expected=%u",
                     (unsigned)s_lcd_sent_pixel_bytes, (unsigned)s_lcd_expected_pixel_bytes);
            (void)lcd_end_data();
        }
        s_lcd_streaming = false;
    }
}
