#ifndef ADC_H
#define ADC_H

#include "esp_err.h"

// ADC 配置
#define ADC_CHANNEL ADC1_CHANNEL_4  // GPIO32
#define ADC_ATTEN ADC_ATTEN_DB_11   // 0-3.9V 量程
#define ADC_WIDTH ADC_WIDTH_BIT_12  // 12位

// 初始化 ADC
esp_err_t adc_init(void);

// 读取 ADC 值 (0-4095 for 12-bit)
int adc_read_raw(void);

// 读取电压值 (V)
float adc_read_voltage(void);

#endif // ADC_H