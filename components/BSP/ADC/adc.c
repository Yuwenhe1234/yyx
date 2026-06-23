#include "adc.h"
#include "driver/adc.h"
#include "esp_log.h"

static const char *TAG = "ADC";

esp_err_t adc_init(void)
{
    // 配置 ADC1 通道
    esp_err_t ret = adc1_config_width(ADC_WIDTH);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC width: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ADC channel attenuation: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ADC initialized successfully");
    return ESP_OK;
}

int adc_read_raw(void)
{
    return adc1_get_raw(ADC_CHANNEL);
}

float adc_read_voltage(void)
{
    int raw = adc_read_raw();
    // 12位 ADC，使用 11dB 衰减，采集电压映射为 0-3.9V
    // 实际硬件需按输入分压设计，并根据校准结果微调
    return (float)raw * 3.9f / 4095.0f;
}