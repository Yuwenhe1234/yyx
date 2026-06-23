#include "ads1013.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>

static const char *TAG = "ADS1013";

ads1013_handle_t* ads1013_init(void)
{
    ads1013_handle_t* handle = (ads1013_handle_t*)malloc(sizeof(ads1013_handle_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for ADS1013 handle");
        return NULL;
    }

    // Initialize I2C master bus
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
        .sda_io_num = ADS1013_SDA_GPIO,
        .scl_io_num = ADS1013_SCL_GPIO,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &handle->bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // Initialize I2C device
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1013_I2C_ADDR,
        .scl_speed_hz = ADS1013_I2C_FREQ,
    };

    ret = i2c_master_bus_add_device(handle->bus_handle, &dev_config, &handle->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(handle->bus_handle);
        free(handle);
        return NULL;
    }

    // Configure ADS1013 for single-ended measurement
    // OS=1 (start conversion), MUX=0 (AIN0/GND), PGA=2 (±2.048V),
    // MODE=0 (continuous), DR=4 (1600 SPS), COMP_QUE=3 (disabled)
    uint16_t config = (1 << ADS1013_OS_SHIFT)      |  // Start conversion
                      (0 << ADS1013_MUX_SHIFT)     |  // AIN0 single-ended
                      (2 << ADS1013_PGA_SHIFT)     |  // ±2.048V
                      (0 << ADS1013_MODE_SHIFT)    |  // Continuous conversion mode
                      (4 << ADS1013_DR_SHIFT)      |  // 1600 SPS
                      (3 << ADS1013_COMP_QUE_SHIFT);  // Comparator disabled

    ads1013_write_config(handle, config);

    ESP_LOGI(TAG, "ADS1013 initialized successfully");
    return handle;
}

void ads1013_deinit(ads1013_handle_t* handle)
{
    if (!handle) return;

    i2c_master_bus_rm_device(handle->dev_handle);
    i2c_del_master_bus(handle->bus_handle);
    free(handle);

    ESP_LOGI(TAG, "ADS1013 deinitialized");
}

int ads1013_write_config(ads1013_handle_t* handle, uint16_t config)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    uint8_t data[3];
    data[0] = ADS1013_REG_CONFIG;
    data[1] = (config >> 8) & 0xFF;  // MSB
    data[2] = config & 0xFF;          // LSB

    esp_err_t ret = i2c_master_transmit(handle->dev_handle, data, 3, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(ret));
    }
    return ret;
}

int ads1013_read_config(ads1013_handle_t* handle, uint16_t* config)
{
    if (!handle || !config) return ESP_ERR_INVALID_ARG;

    uint8_t reg = ADS1013_REG_CONFIG;
    uint8_t data[2];

    esp_err_t ret = i2c_master_transmit_receive(handle->dev_handle, &reg, 1, data, 2, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read config: %s", esp_err_to_name(ret));
        return ret;
    }

    *config = ((uint16_t)data[0] << 8) | data[1];
    return ESP_OK;
}

int ads1013_read_raw(ads1013_handle_t* handle, uint16_t* raw_value)
{
    if (!handle || !raw_value) return ESP_ERR_INVALID_ARG;

    uint8_t reg = ADS1013_REG_CONVERSION;
    uint8_t data[2];

    // Request conversion
    esp_err_t ret = i2c_master_transmit_receive(handle->dev_handle, &reg, 1, data, 2, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read conversion: %s", esp_err_to_name(ret));
        return ret;
    }

    // ADS1013 returns 12-bit value in upper 12 bits (right-aligned in 16-bit register)
    *raw_value = ((uint16_t)data[0] << 8) | data[1];
    *raw_value = (*raw_value >> 4) & 0xFFF;  // Extract 12-bit value

    return ESP_OK;
}

int ads1013_read_voltage(ads1013_handle_t* handle, int16_t* voltage_mv, uint16_t pga_range)
{
    if (!handle || !voltage_mv) return ESP_ERR_INVALID_ARG;

    uint16_t raw_value;
    esp_err_t ret = ads1013_read_raw(handle, &raw_value);
    if (ret != ESP_OK) return ret;

    // Convert raw 12-bit value to voltage
    // ADS1013 uses bipolar input, so convert to signed value
    int16_t signed_value = (int16_t)raw_value;
    if (raw_value & 0x800) {  // Sign bit
        signed_value = -(int16_t)(0x1000 - raw_value);
    }

    // Calculate voltage: (value / 2048) * pga_range
    // Using integer math: (value * pga_range) / 2048
    *voltage_mv = (signed_value * pga_range) / 2048;

    return ESP_OK;
}
