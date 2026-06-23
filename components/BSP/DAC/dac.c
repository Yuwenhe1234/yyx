#include "dac.h"
#include "driver/dac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "DAC";

static dac_output_type_t current_type = DAC_OUTPUT_DC;
static float param1 = 0.0f; // DC: voltage
static float param2 = 0.0f; // reserved
static esp_timer_handle_t dac_timer_handle = NULL;

static const int DAC_SAMPLE_RATE_HZ = 4000;
static const int DAC_OUTPUT_FREQUENCY_HZ = 40;
static const int DAC_TIMER_PERIOD_US = 1000000 / DAC_SAMPLE_RATE_HZ;

static void dac_timer_cb(void *arg)
{
    (void)arg;
    float output_voltage = 0.0f;
    uint64_t now_us = esp_timer_get_time();
    uint64_t period_us = 1000000 / DAC_OUTPUT_FREQUENCY_HZ;

    switch (current_type) {
        case DAC_OUTPUT_DC: {
            float dc_voltage = param1;
            if (dc_voltage < 0.0f) dc_voltage = 0.0f;
            if (dc_voltage > 3.3f) dc_voltage = 3.0f;
            output_voltage = dc_voltage;
            break;
        }

        case DAC_OUTPUT_SINE: {
            float phase = (float)(now_us % period_us) / (float)period_us;
            output_voltage = 1.0f + 1.1f * sinf(2.0f * M_PI * phase);
            if (output_voltage < 0.0f) output_voltage = 0.0f;
            if (output_voltage > 2.05f) output_voltage = 2.05f;
            break;
        }

        case DAC_OUTPUT_PULSE: {
            uint64_t half_period_us = period_us / 2;
            output_voltage = ((now_us % period_us) < half_period_us) ? 2.0f : 0.0f;
            break;
        }

        default:
            output_voltage = 0.0f;
            break;
    }

    uint8_t dac_value = (uint8_t)(output_voltage * 255.0f / 3.3f);
    dac_output_voltage(DAC_CHANNEL, dac_value);
}

esp_err_t dac_init(void)
{
    esp_err_t ret = dac_output_enable(DAC_CHANNEL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable DAC: %s", esp_err_to_name(ret));
        return ret;
    }

    if (dac_timer_handle == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &dac_timer_cb,
            .name = "dac_timer"
        };
        ret = esp_timer_create(&timer_args, &dac_timer_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create DAC timer: %s", esp_err_to_name(ret));
            dac_output_disable(DAC_CHANNEL);
            return ret;
        }

        ret = esp_timer_start_periodic(dac_timer_handle, DAC_TIMER_PERIOD_US);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start DAC timer: %s", esp_err_to_name(ret));
            esp_timer_delete(dac_timer_handle);
            dac_timer_handle = NULL;
            dac_output_disable(DAC_CHANNEL);
            return ret;
        }
    }

    ESP_LOGI(TAG, "DAC initialized successfully");
    return ESP_OK;
}

esp_err_t dac_set_output(dac_output_type_t type, float p1, float p2)
{
    current_type = type;
    param1 = p1;
    param2 = p2;

    ESP_LOGI(TAG, "DAC output set to type %d with params %.2f, %.2f", type, p1, p2);
    return ESP_OK;
}

esp_err_t dac_stop(void)
{
    if (dac_timer_handle != NULL) {
        esp_timer_stop(dac_timer_handle);
        esp_timer_delete(dac_timer_handle);
        dac_timer_handle = NULL;
    }

    dac_output_disable(DAC_CHANNEL);
    ESP_LOGI(TAG, "DAC stopped");
    return ESP_OK;
}