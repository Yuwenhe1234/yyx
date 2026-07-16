#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- ADC 环形缓冲区 ---------- */
#define ADC_RING_SIZE 24

typedef struct {
    uint16_t raw[ADC_RING_SIZE];
    int16_t  voltage_mv[ADC_RING_SIZE];
    volatile uint8_t head;
    uint8_t  tail;
    volatile uint8_t count;
} adc_ring_buf_t;

typedef struct {
    uint16_t adc_raw;
    int16_t  adc_voltage_mv;
    uint16_t dac_value;
    uint8_t  dac_mode;
    uint16_t dac_param;
    bool     adc_valid;
    bool     dac_valid;
} sensor_data_t;

typedef void (*bluetooth_command_cb_t)(const char *command, void *user_ctx);

esp_err_t bluetooth_init(void);
esp_err_t bluetooth_send_key_event(uint8_t pressed);
bool bluetooth_is_connected(void);
bool bluetooth_is_streaming_enabled(void);
esp_err_t bluetooth_send_sensor_data(const sensor_data_t *data);
esp_err_t bluetooth_push_adc_sample(uint16_t raw, int16_t voltage_mv);
void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *user_ctx);
esp_err_t bluetooth_notify_text(const char *text);

#ifdef __cplusplus
}
#endif
