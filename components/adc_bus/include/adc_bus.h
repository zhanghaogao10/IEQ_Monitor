#pragma once

#include "esp_err.h"
#include "hal/adc_types.h"

/* ADC1 共享访问层：避免按键和传感器重复创建 ADC oneshot 单元。 */
esp_err_t adc_bus_init(void);
esp_err_t adc_bus_config_channel(adc_channel_t channel);
esp_err_t adc_bus_config_channel_atten(adc_channel_t channel, adc_atten_t atten);
esp_err_t adc_bus_read_raw(adc_channel_t channel, int *raw);
/* 在同一把总线锁内完成丢弃、采样和去极值平均，避免通道切换造成串扰。 */
esp_err_t adc_bus_read_raw_filtered(adc_channel_t channel,
                                    int discard_count,
                                    int sample_count,
                                    int trim_count,
                                    int *raw);
esp_err_t adc_bus_raw_to_mv(int raw, int *mv);
esp_err_t adc_bus_raw_to_mv_for_channel(adc_channel_t channel, int raw, int *mv);
