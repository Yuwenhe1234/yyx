#ifndef __DAC7311_H__
#define __DAC7311_H__

#include "driver/spi_master.h"

#define DAC7311_SPI_HOST        SPI2_HOST
#define DAC7311_SCLK_GPIO       18
#define DAC7311_MOSI_GPIO       23
#define DAC7311_MISO_GPIO       19
#define DAC7311_CS_GPIO         5
#define DAC7311_SPI_FREQ        1000000  // 1 MHz (up to 50 MHz supported)

// DAC7311 is a 12-bit DAC with SPI interface
#define DAC7311_RESOLUTION_BITS 12
#define DAC7311_MAX_VALUE       ((1 << DAC7311_RESOLUTION_BITS) - 1)  // 4095

// DAC7311 Serial Data Format:
// Format: 16 bits
// Bits [15:14]: Power control (00 = Normal operation, 10 = Power down)
// Bits [13:12]: Don't care
// Bits [11:0]: 12-bit DAC data

#define DAC7311_CMD_NORMAL      0x00
#define DAC7311_CMD_POWERDOWN   0x02

typedef struct {
    spi_device_handle_t spi_handle;
} dac7311_handle_t;

/**
 * Initialize DAC7311 SPI DAC
 * @return handle to DAC7311 device, NULL if failed
 */
dac7311_handle_t* dac7311_init(void);

/**
 * Deinitialize DAC7311
 * @param handle Device handle
 */
void dac7311_deinit(dac7311_handle_t* handle);

/**
 * Write 12-bit value to DAC7311
 * @param handle Device handle
 * @param value 12-bit DAC value (0-4095)
 * @return ESP_OK on success, error code otherwise
 */
int dac7311_write(dac7311_handle_t* handle, uint16_t value);

/**
 * Write voltage to DAC7311
 * @param handle Device handle
 * @param voltage_mv Output voltage in millivolts
 * @param vref_mv Reference voltage in millivolts (e.g., 5000 for 5V ref)
 * @return ESP_OK on success, error code otherwise
 */
int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv);

/**
 * Power down DAC7311
 * @param handle Device handle
 * @return ESP_OK on success, error code otherwise
 */
int dac7311_power_down(dac7311_handle_t* handle);

/**
 * Power up DAC7311 (normal operation)
 * @param handle Device handle
 * @return ESP_OK on success, error code otherwise
 */
int dac7311_power_up(dac7311_handle_t* handle);

#endif /* __DAC7311_H__ */
