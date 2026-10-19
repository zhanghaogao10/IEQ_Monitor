#pragma once

#include "hal/adc_types.h"
#include "driver/gpio.h"

/* 应用层统一配置：任务周期、默认阈值和硬件引脚映射。 */
#define APP_SENSOR_TASK_PERIOD_MS      1000
#define APP_UI_TASK_PERIOD_MS          200
#define APP_KEY_TASK_PERIOD_MS         20
#define APP_ENABLE_ALARM               1

#define APP_DEFAULT_TEMP_HIGH_C        35.0f
#define APP_DEFAULT_HUMIDITY_HIGH_RH   80.0f
#define APP_DEFAULT_CO_HIGH_PPM        100.0f
#define APP_DEFAULT_CO2_HIGH_PPM       3000.0f
#define APP_DEFAULT_ALARM_DURATION_MIN 1U

#define APP_GPIO_BAT_MEAS_ADC          GPIO_NUM_0
#define APP_GPIO_BEEP_EN               GPIO_NUM_1
#define APP_GPIO_LCD_RS                GPIO_NUM_2
#define APP_GPIO_SENSOR_SIOUT          GPIO_NUM_3
#define APP_GPIO_ADC_KEY               GPIO_NUM_4
#define APP_GPIO_LCD_CS                GPIO_NUM_5
#define APP_GPIO_LCD_SDA               GPIO_NUM_6
#define APP_GPIO_LCD_SCL               GPIO_NUM_7
#define APP_GPIO_I2C_SDA               GPIO_NUM_8
#define APP_GPIO_I2C_SCL               GPIO_NUM_9
#define APP_GPIO_LCD_BL                GPIO_NUM_10
#define APP_GPIO_ALARM_OUT             GPIO_NUM_NC
#define APP_KEY_ADC_CHANNEL            ADC_CHANNEL_4
#define APP_ME2_CO_ADC_CHANNEL         ADC_CHANNEL_3

/* TPT29554A IO 扩展器引脚定义。 */
#define APP_IO_EXPANDER_I2C_ADDR       0x20
#define APP_IO_EXPANDER_PIN_CHRG       0
#define APP_IO_EXPANDER_PIN_STDBY      1
#define APP_IO_EXPANDER_PIN_KEY_CHECK  7
#define APP_IO_EXPANDER_PIN_SW_IO      6
#define APP_IO_EXPANDER_PIN_LCD_RESET  5
