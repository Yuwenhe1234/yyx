#ifndef __ADS1013_H__
#define __ADS1013_H__

#include "driver/i2c_master.h"

#define ADS1013_I2C_ADDR    0x48  // Default I2C address (ADDR pin to GND)
#define ADS1013_I2C_PORT    I2C_NUM_0
#define ADS1013_SCL_GPIO    22
#define ADS1013_SDA_GPIO    21
#define ADS1013_I2C_FREQ    100000  // 100 kHz

// ADS1013 Register addresses
#define ADS1013_REG_CONVERSION  0x00
#define ADS1013_REG_CONFIG      0x01
#define ADS1013_REG_LO_THRESH   0x02
#define ADS1013_REG_HI_THRESH   0x03

// Config register bit positions
#define ADS1013_OS_SHIFT        15
#define ADS1013_MUX_SHIFT       12
#define ADS1013_PGA_SHIFT       9
#define ADS1013_MODE_SHIFT      8
#define ADS1013_DR_SHIFT        5
#define ADS1013_COMP_MODE_SHIFT 4
#define ADS1013_COMP_POL_SHIFT  3
#define ADS1013_COMP_LAT_SHIFT  2
#define ADS1013_COMP_QUE_SHIFT  0

// Data rate options (SPS - samples per second)
#define ADS1013_DR_128SPS   0x00
#define ADS1013_DR_250SPS   0x01
#define ADS1013_DR_490SPS   0x02
#define ADS1013_DR_920SPS   0x03
#define ADS1013_DR_1600SPS  0x04
#define ADS1013_DR_2400SPS  0x05
#define ADS1013_DR_3300SPS  0x06

// PGA options (Programmable Gain Amplifier)
#define ADS1013_PGA_6144MV  0x00  // +/- 6.144V
#define ADS1013_PGA_4096MV  0x01  // +/- 4.096V
#define ADS1013_PGA_2048MV  0x02  // +/- 2.048V (default)
#define ADS1013_PGA_1024MV  0x03  // +/- 1.024V
#define ADS1013_PGA_512MV   0x04  // +/- 0.512V
#define ADS1013_PGA_256MV   0x05  // +/- 0.256V

typedef struct {
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
} ads1013_handle_t;

/**
 * Initialize ADS1013 I2C ADC
 * @return handle to ADS1013 device, NULL if failed
 */
ads1013_handle_t* ads1013_init(void);

/**
 * Deinitialize ADS1013
 * @param handle Device handle
 */
void ads1013_deinit(ads1013_handle_t* handle);

/**
 * Read raw ADC value from ADS1013
 * @param handle Device handle
 * @param raw_value Pointer to store 12-bit ADC value
 * @return ESP_OK on success, error code otherwise
 */
int ads1013_read_raw(ads1013_handle_t* handle, uint16_t* raw_value);

/**
 * Read ADC voltage in millivolts
 * @param handle Device handle
 * @param voltage_mv Pointer to store voltage value
 * @param pga_range PGA range in mV (e.g., 6144, 4096, 2048, etc.)
 * @return ESP_OK on success, error code otherwise
 */
int ads1013_read_voltage(ads1013_handle_t* handle, int16_t* voltage_mv, uint16_t pga_range);

/**
 * Write configuration register
 * @param handle Device handle
 * @param config Configuration value (16-bit)
 * @return ESP_OK on success, error code otherwise
 */
int ads1013_write_config(ads1013_handle_t* handle, uint16_t config);

/**
 * Read configuration register
 * @param handle Device handle
 * @param config Pointer to store configuration value
 * @return ESP_OK on success, error code otherwise
 */
int ads1013_read_config(ads1013_handle_t* handle, uint16_t* config);

#endif /* __ADS1013_H__ */
