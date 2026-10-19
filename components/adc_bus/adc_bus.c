#include "adc_bus.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define ADC_BUS_CHANNEL_COUNT 10
#define ADC_BUS_ATTEN_COUNT   4

static const char *TAG = "adc_bus";
static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle[ADC_BUS_ATTEN_COUNT];
static bool s_cali_ready[ADC_BUS_ATTEN_COUNT];
static bool s_channel_configured[ADC_BUS_CHANNEL_COUNT];
static adc_atten_t s_channel_atten[ADC_BUS_CHANNEL_COUNT];
/* ADC1 被 CO 传感器任务和按键任务共享，使用互斥锁保证配置/采样不会并发冲突。 */
static SemaphoreHandle_t s_adc_lock;

static size_t adc_bus_atten_index(adc_atten_t atten)
{
    switch (atten) {
    case ADC_ATTEN_DB_0:
        return 0;
    case ADC_ATTEN_DB_2_5:
        return 1;
    case ADC_ATTEN_DB_6:
        return 2;
    case ADC_ATTEN_DB_12:
    default:
        return 3;
    }
}

static esp_err_t adc_bus_get_cali(adc_atten_t atten, adc_cali_handle_t *handle, bool *ready)
{
    if (handle == NULL || ready == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t index = adc_bus_atten_index(atten);
    if (s_cali_handle[index] == NULL) {
        adc_cali_curve_fitting_config_t cali_config = {
            .unit_id = ADC_UNIT_1,
            .atten = atten,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        s_cali_ready[index] = adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle[index]) == ESP_OK;
    }

    *handle = s_cali_handle[index];
    *ready = s_cali_ready[index];
    return ESP_OK;
}

esp_err_t adc_bus_init(void)
{
    if (s_adc_handle != NULL) {
        return ESP_OK;
    }

    /* 使用静态互斥锁，避免动态分配内存；ADC 总线初始化时只创建一次。 */
    static StaticSemaphore_t lock_buffer;
    s_adc_lock = xSemaphoreCreateMutexStatic(&lock_buffer);
    if (s_adc_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_config, &s_adc_handle), TAG, "create ADC1 failed");
    return ESP_OK;
}

esp_err_t adc_bus_config_channel(adc_channel_t channel)
{
    return adc_bus_config_channel_atten(channel, ADC_ATTEN_DB_12);
}

esp_err_t adc_bus_config_channel_atten(adc_channel_t channel, adc_atten_t atten)
{
    ESP_RETURN_ON_ERROR(adc_bus_init(), TAG, "init ADC bus failed");
    if (channel >= ADC_BUS_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 通道配置也必须加锁，避免一个任务改变衰减时另一个任务正在采样。 */
    xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    if (s_channel_configured[channel] && s_channel_atten[channel] == atten) {
        xSemaphoreGive(s_adc_lock);
        return ESP_OK;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = atten,
    };
    esp_err_t ret = adc_oneshot_config_channel(s_adc_handle, channel, &config);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_adc_lock);
        ESP_LOGE(TAG, "config ADC channel failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_channel_configured[channel] = true;
    s_channel_atten[channel] = atten;
    xSemaphoreGive(s_adc_lock);
    return ESP_OK;
}

esp_err_t adc_bus_read_raw(adc_channel_t channel, int *raw)
{
    if (raw == NULL || channel >= ADC_BUS_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* 已配置的通道必须保留自己的衰减；CO 通道使用 DB_0，按键通道使用 DB_12。 */
    if (!s_channel_configured[channel]) {
        ESP_RETURN_ON_ERROR(adc_bus_config_channel(channel), TAG, "config ADC channel failed");
    }
    /* CO 使用 ADC1_CH3，按键使用 ADC1_CH4；所有 ADC1 读取统一串行化。 */
    xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    esp_err_t ret = adc_oneshot_read(s_adc_handle, channel, raw);
    xSemaphoreGive(s_adc_lock);
    return ret;
}

esp_err_t adc_bus_read_raw_filtered(adc_channel_t channel,
                                    int discard_count,
                                    int sample_count,
                                    int trim_count,
                                    int *raw)
{
    if (raw == NULL || channel >= ADC_BUS_CHANNEL_COUNT ||
        discard_count < 0 || sample_count <= 0 || trim_count < 0 ||
        trim_count * 2 >= sample_count) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_channel_configured[channel]) {
        ESP_RETURN_ON_ERROR(adc_bus_config_channel(channel), TAG, "config ADC channel failed");
    }

    int samples[sample_count];
    xSemaphoreTake(s_adc_lock, portMAX_DELAY);

    /* 丢弃切换 ADC 通道后的前几次结果，避免采样电容残留影响。 */
    for (int i = 0; i < discard_count; i++) {
        esp_err_t ret = adc_oneshot_read(s_adc_handle, channel, &samples[0]);
        if (ret != ESP_OK) {
            xSemaphoreGive(s_adc_lock);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    for (int i = 0; i < sample_count; i++) {
        esp_err_t ret = adc_oneshot_read(s_adc_handle, channel, &samples[i]);
        if (ret != ESP_OK) {
            xSemaphoreGive(s_adc_lock);
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    xSemaphoreGive(s_adc_lock);

    /* 插入排序后去掉两端异常值，再求平均，避免单次跳变抬高 CO。 */
    for (int i = 1; i < sample_count; i++) {
        int value = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > value) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = value;
    }

    int64_t sum = 0;
    for (int i = trim_count; i < sample_count - trim_count; i++) {
        sum += samples[i];
    }
    *raw = (int)(sum / (sample_count - trim_count * 2));
    return ESP_OK;
}

esp_err_t adc_bus_raw_to_mv(int raw, int *mv)
{
    return adc_bus_raw_to_mv_for_channel(ADC_CHANNEL_0, raw, mv);
}

esp_err_t adc_bus_raw_to_mv_for_channel(adc_channel_t channel, int raw, int *mv)
{
    if (mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (channel >= ADC_BUS_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_atten_t atten = s_channel_configured[channel] ? s_channel_atten[channel] : ADC_ATTEN_DB_12;
    adc_cali_handle_t handle = NULL;
    bool ready = false;
    ESP_RETURN_ON_ERROR(adc_bus_get_cali(atten, &handle, &ready), TAG, "get ADC calibration failed");
    if (ready) {
        return adc_cali_raw_to_voltage(handle, raw, mv);
    }

    uint32_t full_scale_mv = (atten == ADC_ATTEN_DB_0) ? 950 : 3300;
    *mv = (raw * full_scale_mv) / 4095;
    return ESP_OK;
}
