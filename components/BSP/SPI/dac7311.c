#include "dac7311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "stdlib.h"

static const char *TAG = "DAC7311";

dac7311_handle_t* dac7311_init(void)
{
    dac7311_handle_t* handle = (dac7311_handle_t*)malloc(sizeof(dac7311_handle_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for DAC7311 handle");
        return NULL;
    }

    // Initialize SPI bus
    spi_bus_config_t bus_config = {
        .mosi_io_num = DAC7311_MOSI_GPIO,
        .miso_io_num = DAC7311_MISO_GPIO,
        .sclk_io_num = DAC7311_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,  // Small transfers only
    };

    esp_err_t ret = spi_bus_initialize(DAC7311_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // Attach DAC7311 device to SPI bus
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = DAC7311_SPI_FREQ,
        .mode = 1,  // CPOL=0, CPHA=1
        .spics_io_num = DAC7311_CS_GPIO,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ret = spi_bus_add_device(DAC7311_SPI_HOST, &dev_config, &handle->spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(DAC7311_SPI_HOST);
        free(handle);
        return NULL;
    }

    ESP_LOGI(TAG, "DAC7311 initialized successfully");
    
    // Power up and set to 0V
    dac7311_write(handle, 0);

    return handle;
}

void dac7311_deinit(dac7311_handle_t* handle)
{
    if (!handle) return;

    spi_bus_remove_device(handle->spi_handle);
    spi_bus_free(DAC7311_SPI_HOST);
    free(handle);

    ESP_LOGI(TAG, "DAC7311 deinitialized");
}

int dac7311_write(dac7311_handle_t* handle, uint16_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    if (value > DAC7311_MAX_VALUE) {
        ESP_LOGW(TAG, "DAC value out of range, clamping to max");
        value = DAC7311_MAX_VALUE;
    }

    // Format: [POWERDOWN_BITS(2) | UNUSED(2) | DAC_DATA(12)]
    // For normal operation: 0x0000 | 0x0000 | DAC_DATA
    uint16_t tx_data = (DAC7311_CMD_NORMAL << 14) | (value & 0xFFF);

    // Convert to big-endian for transmission
    uint8_t data[2];
    data[0] = (tx_data >> 8) & 0xFF;
    data[1] = tx_data & 0xFF;

    spi_transaction_t trans = {
        .length = 16,  // 16 bits
        .tx_buffer = data,
    };

    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write DAC value: %s", esp_err_to_name(ret));
    }

    return ret;
}

int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // Convert voltage to 12-bit value
    // value = (voltage_mv / vref_mv) * 4095
    uint32_t dac_value = ((uint32_t)voltage_mv * DAC7311_MAX_VALUE) / vref_mv;

    if (dac_value > DAC7311_MAX_VALUE) {
        dac_value = DAC7311_MAX_VALUE;
        ESP_LOGW(TAG, "Voltage exceeds reference, clamping to max");
    }

    return dac7311_write(handle, (uint16_t)dac_value);
}

int dac7311_power_down(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // Set power down mode and clear DAC value
    uint16_t tx_data = (DAC7311_CMD_POWERDOWN << 14);

    uint8_t data[2];
    data[0] = (tx_data >> 8) & 0xFF;
    data[1] = tx_data & 0xFF;

    spi_transaction_t trans = {
        .length = 16,
        .tx_buffer = data,
    };

    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power down DAC: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "DAC7311 powered down");
    }

    return ret;
}

int dac7311_power_up(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // Power up and set to 0V
    return dac7311_write(handle, 0);
}
