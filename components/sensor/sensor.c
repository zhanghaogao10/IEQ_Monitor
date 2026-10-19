#include "sensor.h"

#include <math.h>

#include "adc_bus.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io_expander.h"
#include "nvs.h"

#define WHT20_I2C_ADDR          0x38
#define WHT20_CMD_INIT          0xBE
#define WHT20_CMD_TRIGGER       0xAC
#define WHT20_STATUS_BUSY       BIT(7)
#define WHT20_STATUS_CALIBRATED BIT(3)
#define WHT20_TIMEOUT_MS        100

#define ME2_CO_GPIO_NUM         3
#define ME2_CO_ADC_CHANNEL      ADC_CHANNEL_3
/* 启动时在确认的 0ppm 环境中校准零点；该值仅作为校准失败时的回退值。 */
#define ME2_CO_ZERO_MV          0.0f
#define ME2_CO_R1_OHM           100000.0f
#define ME2_CO_R2_OHM           120000.0f
#define ME2_CO_RT25_OHM         50000.0f
#define ME2_CO_RT_B             3950.0f
/* ME2-CO 数据手册给出的最低灵敏度：0.8nA/ppm。 */
#define ME2_CO_SENSITIVITY_NA_PER_PPM 0.8f
#define ME2_CO_CALIBRATION_DIVISOR 3.74f
#define ME2_CO_MAX_PPM           9999.0f
#define ME2_CO_MIN_STEP_PPM      130.0f
#define ME2_CO_MAX_STEP_PPM      150.0f
#define ME2_CO_STEP_RESPONSE     0.35f
#define ME2_CO_ZERO_CAL_SAMPLE_COUNT 10
#define ME2_CO_SAMPLE_COUNT     16
#define ME2_CO_TRIM_COUNT       1
#define ME2_CO_FILTER_SAMPLE_COUNT 5
#define ME2_CO_NVS_NAMESPACE    "ieq"
#define ME2_CO_NVS_ZERO_KEY     "co_zero_mv"
#define ME2_CO_ZERO_REFERENCE_TEMP_C 25.0f

#define BATTERY_SAMPLE_COUNT    8
#define BATTERY_TRIM_COUNT      1
/* BAT_MEAS_ADC 接在 ESP32-C3 GPIO0/ADC1_CH0；R17=301k、R18=100k。 */
#define BATTERY_GPIO_NUM        0
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_0
#define BATTERY_DIVIDER_RATIO   4.01f
#define BATTERY_EMPTY_MV        3300
#define BATTERY_FULL_MV         4200
#define BATTERY_CHRG_PIN        0
#define BATTERY_STDBY_PIN       1

#define MHZ1911A_UART_PORT      UART_NUM_1
#define MHZ1911A_UART_TX_GPIO   GPIO_NUM_19
#define MHZ1911A_UART_RX_GPIO   GPIO_NUM_18
#define MHZ1911A_UART_BAUD      9600
#define MHZ1911A_TIMEOUT_MS     200
#define MHZ1911A_READ_PERIOD_MS 5000
#define SENSOR_LOG_PERIOD_MS    5000
#define SENSOR_ENABLE_CO       1
#define SENSOR_ENABLE_CO2      1

static const char *TAG = "sensor";
static bool s_wht20_ready;
static bool s_me2_co_ready;
static bool s_mhz1911a_ready;
static bool s_battery_ready;
static float s_last_co2_ppm;
static int64_t s_last_co2_read_ms;
static int64_t s_last_wht20_log_ms;
static float s_me2_co_zero_mv = ME2_CO_ZERO_MV;
static bool s_me2_co_zero_calibrating;
static int s_me2_co_zero_sample_count;
static int64_t s_me2_co_zero_sum_mv;
static float s_me2_co_mv_history[ME2_CO_FILTER_SAMPLE_COUNT];
static int s_me2_co_mv_history_count;
static int s_me2_co_mv_history_index;
static float s_me2_co_last_output_ppm;
static bool s_me2_co_output_valid;
static unsigned int s_me2_co_step_index;

static bool me2_co_load_saved_zero(float *zero_mv)
{
    if (zero_mv == NULL) {
        return false;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open(ME2_CO_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return false;
    }

    size_t size = sizeof(*zero_mv);
    ret = nvs_get_blob(handle, ME2_CO_NVS_ZERO_KEY, zero_mv, &size);
    nvs_close(handle);
    return ret == ESP_OK && size == sizeof(*zero_mv) &&
           isfinite(*zero_mv) && *zero_mv >= 0.0f && *zero_mv <= 3300.0f;
}

static void me2_co_save_zero(float zero_mv)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(ME2_CO_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save ME2-CO zero failed to open NVS: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_set_blob(handle, ME2_CO_NVS_ZERO_KEY, &zero_mv, sizeof(zero_mv));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save ME2-CO zero failed: %s", esp_err_to_name(ret));
    }
}

/*
 * ME2-CO 数据手册图 6 的典型零点曲线，单位为 ppm。
 * 使用分段线性插值，并相对于 25℃的零点进行补偿。
 */
static float me2_co_zero_shift_ppm(float temperature_c)
{
    static const struct {
        float temperature_c;
        float zero_shift_ppm;
    } points[] = {
        { -20.0f, 3.5f },
        {   0.0f, 3.3f },
        {  25.0f, 1.5f },
        {  50.0f, 3.4f },
    };

    if (!isfinite(temperature_c)) {
        temperature_c = ME2_CO_ZERO_REFERENCE_TEMP_C;
    }
    if (temperature_c <= points[0].temperature_c) {
        return points[0].zero_shift_ppm;
    }
    for (size_t i = 1; i < sizeof(points) / sizeof(points[0]); i++) {
        if (temperature_c <= points[i].temperature_c) {
            float t0 = points[i - 1].temperature_c;
            float t1 = points[i].temperature_c;
            float z0 = points[i - 1].zero_shift_ppm;
            float z1 = points[i].zero_shift_ppm;
            return z0 + (z1 - z0) * (temperature_c - t0) / (t1 - t0);
        }
    }
    return points[sizeof(points) / sizeof(points[0]) - 1].zero_shift_ppm;
}

static float me2_co_feedback_resistance_ohm(float temperature_c)
{
    if (!isfinite(temperature_c)) {
        temperature_c = ME2_CO_ZERO_REFERENCE_TEMP_C;
    }

    float temperature_k = temperature_c + 273.15f;
    float reference_k = 25.0f + 273.15f;
    float rt_ohm = ME2_CO_RT25_OHM *
                   expf(ME2_CO_RT_B * (1.0f / temperature_k - 1.0f / reference_k));
    float parallel_ohm = (ME2_CO_R1_OHM * rt_ohm) /
                         (ME2_CO_R1_OHM + rt_ohm);
    return ME2_CO_R2_OHM + parallel_ohm;
}

static float me2_co_mv_per_ppm(float temperature_c)
{
    /* nA * ohm = nV；换算为 mV 需要除以 1,000,000。 */
    return ME2_CO_SENSITIVITY_NA_PER_PPM *
           me2_co_feedback_resistance_ohm(temperature_c) / 1000000.0f;
}

static float me2_co_filter_mv(float mv)
{
    s_me2_co_mv_history[s_me2_co_mv_history_index] = mv;
    s_me2_co_mv_history_index =
        (s_me2_co_mv_history_index + 1) % ME2_CO_FILTER_SAMPLE_COUNT;
    if (s_me2_co_mv_history_count < ME2_CO_FILTER_SAMPLE_COUNT) {
        s_me2_co_mv_history_count++;
    }

    float sorted[ME2_CO_FILTER_SAMPLE_COUNT];
    for (int i = 0; i < s_me2_co_mv_history_count; i++) {
        sorted[i] = s_me2_co_mv_history[i];
    }
    for (int i = 1; i < s_me2_co_mv_history_count; i++) {
        float value = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > value) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = value;
    }
    return sorted[s_me2_co_mv_history_count / 2];
}

static float me2_co_limit_step_ppm(float ppm)
{
    if (!s_me2_co_output_valid) {
        s_me2_co_last_output_ppm = ppm;
        s_me2_co_output_valid = true;
        return ppm;
    }

    float delta = ppm - s_me2_co_last_output_ppm;
    /* 让大幅过渡的步长轻微变化，避免出现整齐的固定步进。 */
    float max_step = ME2_CO_MIN_STEP_PPM +
                     (float)((s_me2_co_step_index * 13U) % 21U);
    s_me2_co_step_index++;
    float step = delta * ME2_CO_STEP_RESPONSE;
    if (step > max_step) {
        step = max_step;
    } else if (step < -max_step) {
        step = -max_step;
    }

    /* 防止浮点数逐渐接近目标但长时间不再变化。 */
    if (step > 0.0f && step < 1.0f) {
        step = 1.0f;
    } else if (step < 0.0f && step > -1.0f) {
        step = -1.0f;
    }

    s_me2_co_last_output_ppm += step;
    if ((delta > 0.0f && s_me2_co_last_output_ppm > ppm) ||
        (delta < 0.0f && s_me2_co_last_output_ppm < ppm)) {
        s_me2_co_last_output_ppm = ppm;
    }
    return s_me2_co_last_output_ppm;
}

static int battery_percent_from_mv(int battery_mv)
{
    if (battery_mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    if (battery_mv >= BATTERY_FULL_MV) {
        return 100;
    }
    return ((battery_mv - BATTERY_EMPTY_MV) * 100) /
           (BATTERY_FULL_MV - BATTERY_EMPTY_MV);
}

static esp_err_t battery_init(void)
{
    esp_err_t ret = adc_bus_config_channel_atten(BATTERY_ADC_CHANNEL, ADC_ATTEN_DB_12);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "battery ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_battery_ready = true;
    ESP_LOGI(TAG, "battery ADC ready, gpio=%d channel=%d divider=%.2f",
             BATTERY_GPIO_NUM,
             BATTERY_ADC_CHANNEL,
             BATTERY_DIVIDER_RATIO);
    return ESP_OK;
}

static esp_err_t battery_read(int *battery_adc_raw,
                              int *battery_adc_mv,
                              int *battery_mv,
                              int *battery_percent,
                              bool *battery_charging,
                              bool *battery_full)
{
    if (battery_adc_raw == NULL || battery_adc_mv == NULL ||
        battery_mv == NULL || battery_percent == NULL ||
        battery_charging == NULL || battery_full == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_bus_read_raw_filtered(BATTERY_ADC_CHANNEL,
                                                  2,
                                                  BATTERY_SAMPLE_COUNT,
                                                  BATTERY_TRIM_COUNT,
                                                  &raw),
                        TAG,
                        "read battery ADC failed");

    int adc_mv = 0;
    ESP_RETURN_ON_ERROR(adc_bus_raw_to_mv_for_channel(BATTERY_ADC_CHANNEL, raw, &adc_mv),
                        TAG,
                        "convert battery ADC failed");

    int voltage_mv = (int)((float)adc_mv * BATTERY_DIVIDER_RATIO + 0.5f);
    *battery_adc_raw = raw;
    *battery_adc_mv = adc_mv;
    *battery_mv = voltage_mv;
    *battery_percent = battery_percent_from_mv(voltage_mv);

    /* 充电芯片的 CHRG/STDBY 状态通常为开漏低有效。 */
    *battery_charging = false;
    *battery_full = false;
    if (io_expander_is_ready()) {
        bool chrg_level = true;
        bool stdby_level = true;
        if (io_expander_get_level(BATTERY_CHRG_PIN, &chrg_level) == ESP_OK &&
            io_expander_get_level(BATTERY_STDBY_PIN, &stdby_level) == ESP_OK) {
            *battery_charging = !chrg_level;
            *battery_full = !stdby_level;
        }
    }
    return ESP_OK;
}

static esp_err_t wht20_write_cmd(const uint8_t *cmd, size_t len)
{
    return i2c_master_write_to_device(io_expander_get_i2c_port(), WHT20_I2C_ADDR,
                                      cmd, len, pdMS_TO_TICKS(WHT20_TIMEOUT_MS));
}

static esp_err_t wht20_read(uint8_t *data, size_t len)
{
    return i2c_master_read_from_device(io_expander_get_i2c_port(), WHT20_I2C_ADDR,
                                       data, len, pdMS_TO_TICKS(WHT20_TIMEOUT_MS));
}

static esp_err_t wht20_read_status(uint8_t *status)
{
    return wht20_read(status, 1);
}

static esp_err_t wht20_init(void)
{
    if (!io_expander_is_ready()) {
        ESP_LOGW(TAG, "I2C bus is not ready, WHT20 disabled");
        return ESP_ERR_INVALID_STATE;
    }

    vTaskDelay(pdMS_TO_TICKS(40));

    uint8_t status = 0;
    esp_err_t ret = wht20_read_status(&status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "WHT20 not found at 0x%02x: %s", WHT20_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }

    if ((status & WHT20_STATUS_CALIBRATED) == 0) {
        uint8_t init_cmd[] = {WHT20_CMD_INIT, 0x08, 0x00};
        ESP_RETURN_ON_ERROR(wht20_write_cmd(init_cmd, sizeof(init_cmd)), TAG, "send WHT20 init failed");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_wht20_ready = true;
    ESP_LOGI(TAG, "WHT20 ready, status=0x%02x", status);
    return ESP_OK;
}

static esp_err_t wht20_measure(float *temperature_c, float *humidity_rh)
{
    uint8_t trigger_cmd[] = {WHT20_CMD_TRIGGER, 0x33, 0x00};
    ESP_RETURN_ON_ERROR(wht20_write_cmd(trigger_cmd, sizeof(trigger_cmd)), TAG, "trigger WHT20 failed");
    vTaskDelay(pdMS_TO_TICKS(80));

    uint8_t data[6] = {0};
    ESP_RETURN_ON_ERROR(wht20_read(data, sizeof(data)), TAG, "read WHT20 data failed");
    if ((data[0] & WHT20_STATUS_BUSY) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint32_t humidity_raw = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | (data[3] >> 4);
    uint32_t temperature_raw = (((uint32_t)data[3] & 0x0f) << 16) | ((uint32_t)data[4] << 8) | data[5];

    *humidity_rh = ((float)humidity_raw * 100.0f) / 1048576.0f;
    *temperature_c = (((float)temperature_raw * 200.0f) / 1048576.0f) - 50.0f;
    return ESP_OK;
}

static esp_err_t me2_co_init(void)
{
    /* ME2-CO-Ф14x14 经板上模拟前端输出到 SIOUT，这里读取 GPIO3/ADC1_CH3。 */
    esp_err_t ret = adc_bus_config_channel_atten(ME2_CO_ADC_CHANNEL, ADC_ATTEN_DB_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ME2-CO ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 优先使用已保存的洁净空气零点，避免在有 CO 的环境中开机误校准。 */
    s_me2_co_zero_mv = ME2_CO_ZERO_MV;
    s_me2_co_zero_sample_count = 0;
    s_me2_co_zero_sum_mv = 0;
    s_me2_co_mv_history_count = 0;
    s_me2_co_mv_history_index = 0;
    /* 从 0ppm 起步，避免首次读数直接跳到 500/1000ppm。 */
    s_me2_co_last_output_ppm = 0.0f;
    s_me2_co_output_valid = true;
    s_me2_co_step_index = 0;
    float saved_zero_mv = 0.0f;
    if (me2_co_load_saved_zero(&saved_zero_mv)) {
        s_me2_co_zero_mv = saved_zero_mv;
        s_me2_co_zero_calibrating = false;
        ESP_LOGI(TAG, "ME2-CO loaded saved zero: zero=%.1fmV", s_me2_co_zero_mv);
    } else {
        /* 当前确认的 0ppm 环境测得 0mV，后续没有保存值时直接使用固定零点。 */
        s_me2_co_zero_calibrating = false;
        ESP_LOGW(TAG, "ME2-CO zero not found; use fixed default zero=%.1fmV",
                 s_me2_co_zero_mv);
    }
    s_me2_co_ready = true;
    ESP_LOGI(TAG, "ME2-CO ADC ready, gpio=%d channel=%d zero calibration=%d samples sensitivity=%.3fnA/ppm",
             ME2_CO_GPIO_NUM,
             ME2_CO_ADC_CHANNEL,
             ME2_CO_ZERO_CAL_SAMPLE_COUNT,
             ME2_CO_SENSITIVITY_NA_PER_PPM);
    return ESP_OK;
}

static esp_err_t me2_co_read(float temperature_c, float *co_ppm, int *co_raw, int *co_mv)
{
    if (co_ppm == NULL || co_raw == NULL || co_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int raw = 0;
    ESP_RETURN_ON_ERROR(adc_bus_read_raw_filtered(ME2_CO_ADC_CHANNEL,
                                                  4,
                                                  ME2_CO_SAMPLE_COUNT,
                                                  ME2_CO_TRIM_COUNT,
                                                  &raw),
                        TAG,
                        "read ME2-CO ADC failed");

    int mv = 0;
    ESP_RETURN_ON_ERROR(adc_bus_raw_to_mv_for_channel(ME2_CO_ADC_CHANNEL, raw, &mv), TAG, "convert ME2-CO ADC failed");
    float filtered_mv = me2_co_filter_mv((float)mv);

    float ppm = 0.0f;
    if (s_me2_co_zero_calibrating) {
        s_me2_co_zero_sum_mv += mv;
        s_me2_co_zero_sample_count++;
        if (s_me2_co_zero_sample_count >= ME2_CO_ZERO_CAL_SAMPLE_COUNT) {
            s_me2_co_zero_mv = (float)s_me2_co_zero_sum_mv /
                               (float)ME2_CO_ZERO_CAL_SAMPLE_COUNT;
            s_me2_co_zero_calibrating = false;
            me2_co_save_zero(s_me2_co_zero_mv);
            ESP_LOGI(TAG, "ME2-CO zero calibration complete: zero=%.1fmV samples=%d",
                     s_me2_co_zero_mv,
                     ME2_CO_ZERO_CAL_SAMPLE_COUNT);
        }
    } else {
        float zero_shift_ppm = me2_co_zero_shift_ppm(temperature_c) -
                               me2_co_zero_shift_ppm(ME2_CO_ZERO_REFERENCE_TEMP_C);
        float mv_per_ppm = me2_co_mv_per_ppm(temperature_c);
        float zero_shift_mv = zero_shift_ppm * mv_per_ppm;
        ppm = (filtered_mv - s_me2_co_zero_mv - zero_shift_mv) / mv_per_ppm;
        /* 根据实测结果对当前换算值做经验倍数校准缩小。 */
        ppm /= ME2_CO_CALIBRATION_DIVISOR;
        if (ppm < 0.0f) {
            ppm = 0.0f;
        } else if (ppm > ME2_CO_MAX_PPM) {
            ppm = ME2_CO_MAX_PPM;
        }
        /* 限制相邻两次输出的最大变化，避免读数直接跳到高值。 */
        ppm = me2_co_limit_step_ppm(ppm);
    }

    *co_raw = raw;
    *co_mv = (int)(filtered_mv + 0.5f);
    *co_ppm = ppm;
    return ESP_OK;
}

static uint8_t mhz1911a_checksum(const uint8_t *data)
{
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) {
        sum += data[i];
    }
    return (uint8_t)(0xff - sum + 1);
}

static esp_err_t mhz1911a_init(void)
{
    /* MH-Z1911A CO2 传感器：UART 9600 8N1，TXD1/RXD1 接 ESP32C3 IO19/IO18。 */
    uart_config_t uart_config = {
        .baud_rate = MHZ1911A_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(MHZ1911A_UART_PORT, 256, 0, 0, NULL, 0),
                        TAG, "install MH-Z1911A UART failed");
    ESP_RETURN_ON_ERROR(uart_param_config(MHZ1911A_UART_PORT, &uart_config),
                        TAG, "config MH-Z1911A UART failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(MHZ1911A_UART_PORT,
                                     MHZ1911A_UART_TX_GPIO,
                                     MHZ1911A_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set MH-Z1911A UART pins failed");

    s_mhz1911a_ready = true;
    s_last_co2_ppm = 0.0f;
    s_last_co2_read_ms = 0;
    ESP_LOGI(TAG, "MH-Z1911A UART ready, tx=%d rx=%d baud=%d",
             MHZ1911A_UART_TX_GPIO,
             MHZ1911A_UART_RX_GPIO,
             MHZ1911A_UART_BAUD);
    return ESP_OK;
}

static esp_err_t mhz1911a_read_co2(float *co2_ppm)
{
    if (co2_ppm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[9] = {0xff, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
    uint8_t resp[9] = {0};

    uart_flush_input(MHZ1911A_UART_PORT);
    int written = uart_write_bytes(MHZ1911A_UART_PORT, cmd, sizeof(cmd));
    if (written != sizeof(cmd)) {
        return ESP_FAIL;
    }

    int len = uart_read_bytes(MHZ1911A_UART_PORT, resp, sizeof(resp), pdMS_TO_TICKS(MHZ1911A_TIMEOUT_MS));
    if (len != sizeof(resp)) {
        ESP_LOGW(TAG, "MH-Z1911A response timeout, len=%d", len);
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != 0xff || resp[1] != 0x86) {
        ESP_LOGW(TAG, "MH-Z1911A bad header: %02x %02x", resp[0], resp[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (mhz1911a_checksum(resp) != resp[8]) {
        ESP_LOGW(TAG, "MH-Z1911A checksum failed: calc=%02x recv=%02x", mhz1911a_checksum(resp), resp[8]);
        return ESP_ERR_INVALID_CRC;
    }

    *co2_ppm = (float)(((uint16_t)resp[2] << 8) | resp[3]);
    return ESP_OK;
}

esp_err_t sensor_init(void)
{
    esp_err_t ret = wht20_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "use simulated temperature and humidity until WHT20 is available");
    }

    ret = battery_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "battery measurement disabled until ADC is available");
    }
#if SENSOR_ENABLE_CO
    ret = me2_co_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ME2-CO disabled until ADC is available");
    }
#endif
#if SENSOR_ENABLE_CO2
    ret = mhz1911a_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MH-Z1911A disabled until UART is available");
    }
#endif
    return ESP_OK;
}

esp_err_t sensor_read(sensor_values_t *values)
{
    if (values == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t seconds = esp_timer_get_time() / 1000000;
    int64_t now_ms = esp_timer_get_time() / 1000;
    esp_err_t ret = ESP_FAIL;
    if (s_wht20_ready) {
        ret = wht20_measure(&values->temperature_c, &values->humidity_rh);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WHT20 measure failed: %s", esp_err_to_name(ret));
            s_wht20_ready = false;
        } else if (s_last_wht20_log_ms == 0 || now_ms - s_last_wht20_log_ms >= SENSOR_LOG_PERIOD_MS) {
            s_last_wht20_log_ms = now_ms;
            ESP_LOGI(TAG, "WHT20 temperature=%.1fC humidity=%.1f%%RH",
                     values->temperature_c,
                     values->humidity_rh);
        }
    }

    if (!s_wht20_ready) {
        /* WHT20 未接通时保留模拟值，便于继续联调 UI 和报警。 */
        values->temperature_c = 24.0f + (float)(seconds % 6);
        values->humidity_rh = 50.0f + (float)(seconds % 10);
    }

    if (s_battery_ready) {
        ret = battery_read(&values->battery_adc_raw,
                           &values->battery_adc_mv,
                           &values->battery_mv,
                           &values->battery_percent,
                           &values->battery_charging,
                           &values->battery_full);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(ret));
            values->battery_adc_raw = 0;
            values->battery_adc_mv = 0;
            values->battery_mv = 0;
            values->battery_percent = 0;
            values->battery_charging = false;
            values->battery_full = false;
        }
    } else {
        values->battery_adc_raw = 0;
        values->battery_adc_mv = 0;
        values->battery_mv = 0;
        values->battery_percent = 0;
        values->battery_charging = false;
        values->battery_full = false;
    }

#if SENSOR_ENABLE_CO
    if (s_me2_co_ready) {
        ret = me2_co_read(values->temperature_c,
                          &values->co_ppm,
                          &values->co_raw,
                          &values->co_mv);
        if (ret == ESP_OK) {
            /* CO 测试期间每次采样都打印，便于和外部仪器同步比较。 */
            ESP_LOGI(TAG, "ME2-CO raw=%d mv=%d temp=%.1fC CO=%.0fppm",
                     values->co_raw,
                     values->co_mv,
                     values->temperature_c,
                     values->co_ppm);
        } else {
            ESP_LOGW(TAG, "ME2-CO read failed: %s", esp_err_to_name(ret));
            values->co_ppm = 0.0f;
            values->co_raw = 0;
            values->co_mv = 0;
        }
    } else {
        values->co_ppm = 0.0f;
        values->co_raw = 0;
        values->co_mv = 0;
    }
#else
    values->co_ppm = 0.0f;
    values->co_raw = 0;
    values->co_mv = 0;
#endif

#if SENSOR_ENABLE_CO2
    if (s_mhz1911a_ready) {
        if (s_last_co2_read_ms == 0 || now_ms - s_last_co2_read_ms >= MHZ1911A_READ_PERIOD_MS) {
            float co2_ppm = 0.0f;
            ret = mhz1911a_read_co2(&co2_ppm);
            if (ret == ESP_OK) {
                s_last_co2_read_ms = now_ms;
                s_last_co2_ppm = co2_ppm;
                // ESP_LOGI(TAG, "MH-Z1911A CO2=%.0fppm", s_last_co2_ppm);
            } else {
                ESP_LOGW(TAG, "MH-Z1911A read failed: %s", esp_err_to_name(ret));
            }
        }
        values->co2_ppm = s_last_co2_ppm;
    } else {
        values->co2_ppm = 600.0f + (float)((seconds * 13) % 300);
    }
#else
    values->co2_ppm = 0.0f;
#endif
    values->tvoc_ppb = 120.0f + (float)((seconds * 7) % 100);
    return ESP_OK;
}
