#ifndef __DAC7311_H__
#define __DAC7311_H__

#include "driver/spi_master.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ============================================================
 * SPI 硬件引脚定义
 * ============================================================ */
#define DAC7311_SPI_HOST        SPI2_HOST
#define DAC7311_SCLK_GPIO       18
#define DAC7311_MOSI_GPIO       23
#define DAC7311_MISO_GPIO       19
#define DAC7311_CS_GPIO         5
#define DAC7311_SPI_FREQ        10000000

/* ============================================================
 * DAC 芯片参数（DAC7311/DAC7571, 12-bit, VDD=3.3V）
 * ============================================================ */
#define DAC7311_RESOLUTION_BITS 12
#define DAC7311_MAX_VALUE       ((1 << DAC7311_RESOLUTION_BITS) - 1)
#define DAC_VREF_MV             3300

#define DAC7311_CMD_NORMAL      0x00
#define DAC7311_CMD_POWERDOWN   0x02

/* ============================================================
 * 波形模式
 * ============================================================ */
typedef enum {
    DAC7311_MODE_NONE = 0,
    DAC7311_MODE_SINE,
    DAC7311_MODE_PULSE,
} dac7311_waveform_mode_t;

#define DAC7311_SINE_LUT_SIZE   512

/* ============================================================
 * 设备句柄 — Core 1 GPTimer ISR 直发 SPI
 * ============================================================ */
typedef struct {
    spi_device_handle_t spi_handle;

    dac7311_waveform_mode_t waveform_mode;
    uint64_t waveform_period_us;
    gptimer_handle_t waveform_timer;
    TaskHandle_t waveform_task;                 // Core 1 管理任务

    uint8_t sine_lut[DAC7311_SINE_LUT_SIZE][2];
    uint16_t sine_phase;
    uint16_t sine_freq_hz;
    uint16_t sine_amplitude_mv;

    uint16_t pulse_high_code;
    uint16_t pulse_low_code;
    bool pulse_state;
} dac7311_handle_t;

/* ---- 生命周期 ---- */
dac7311_handle_t* dac7311_init(void);
void dac7311_deinit(dac7311_handle_t* handle);

/* ---- 低层写入 ---- */
int dac7311_write(dac7311_handle_t* handle, uint16_t value);
int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv);

/* ---- 直流 ---- */
int dac7311_set_dc(dac7311_handle_t* handle, uint16_t voltage_mv);

/* ---- 正弦波 ---- */
int dac7311_start_sine(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv);

/* ---- 脉冲波 ---- */
int dac7311_start_pulse(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv);

/* ---- 停止 ---- */
int dac7311_stop_waveform(dac7311_handle_t* handle);

/* ---- 电源 ---- */
int dac7311_power_down(dac7311_handle_t* handle);
int dac7311_power_up(dac7311_handle_t* handle);

#endif /* __DAC7311_H__ */
