#ifndef __DAC7311_H__
#define __DAC7311_H__

#include "driver/spi_master.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ============================================================
 * SPI 硬件引脚定义
 * ============================================================
 * ESP32 通过 SPI2（HSPI 外设）与 DAC 通信，4 线连接（MISO 不用）：
 *   ESP32              DAC（6 脚 SC70 / 8 脚封装）
 *   GPIO18 SCLK  ────→ SCLK   串行时钟
 *   GPIO23 MOSI  ────→ DIN    串行数据
 *   GPIO5  CS    ────→ SYNC   片选（低有效）
 *   GPIO19 MISO  ────→ 无连接
 * ============================================================ */
#define DAC7311_SPI_HOST        SPI2_HOST
#define DAC7311_SCLK_GPIO       18
#define DAC7311_MOSI_GPIO       23
#define DAC7311_MISO_GPIO       19
#define DAC7311_CS_GPIO         5
#define DAC7311_SPI_FREQ        10000000        // 10 MHz（1.6µs/帧，避开 BLE 中断窗口）

/* ============================================================
 * DAC 芯片参数（DAC7311/DAC7571，12 位，VDD 兼参考）
 * ============================================================
 * 输出公式：Vout = VDD × CODE / 4096
 *   VREF = DAC_VREF_MV（芯片实测 825mV）
 *   CODE = mV × 4096 / DAC_VREF_MV
 *   例：500mV → CODE = 500×4096/825 ≈ 2482（满量程 825mV 对应 CODE=4095）
 * ============================================================ */
#define DAC7311_RESOLUTION_BITS 12
#define DAC7311_MAX_VALUE       ((1 << DAC7311_RESOLUTION_BITS) - 1)  // 4095
#define DAC_VREF_MV             3300            // VDD 参考电压（mV）

/* ============================================================
 * 16 位 SPI 数据帧（MSB 先发）
 * ============================================================
 *   DB[15:14] = PD1,PD0（00=正常, 01=1kΩ, 10=100kΩ, 11=高阻）
 *   DB[13:12] = 无关
 *   DB[11:0]  = 12 位 DAC 编码
 * ============================================================ */
#define DAC7311_CMD_NORMAL      0x00            // PD=00 → 正常工作
#define DAC7311_CMD_POWERDOWN   0x02            // PD=10 → 100kΩ 到地

/* ============================================================
 * 波形模式枚举
 * ============================================================ */
typedef enum {
    DAC7311_MODE_NONE = 0,                      // 无波形（DC 模式）
    DAC7311_MODE_SINE,                          // 正弦波
    DAC7311_MODE_PULSE,                         // 脉冲波
} dac7311_waveform_mode_t;

/* ============================================================
 * 正弦波参数
 * ============================================================
 * 256 点 LUT，预格式化为 2 字节 SPI 数据（PD=00 嵌入）。
 * 样本率 = 频率 × 256，硬件定时器驱动。
 * 例：40Hz → 10240 样本/秒 → 间隔 97.6µs
 * ============================================================ */
#define DAC7311_SINE_LUT_SIZE   512             // LUT 大小（512 点，40Hz 时 20480 sps，平滑）

/* ============================================================
 * 设备句柄
 * ============================================================ */
typedef struct {
    spi_device_handle_t spi_handle;             // SPI 设备句柄

    // 波形状态
    dac7311_waveform_mode_t waveform_mode;      // 当前模式
    gptimer_handle_t waveform_timer;            // GPTimer 硬件定时器
    TaskHandle_t waveform_task;                 // 波形生成任务

    // 正弦波专用
    uint8_t sine_lut[DAC7311_SINE_LUT_SIZE][2]; // 预格式化 LUT
    uint16_t sine_phase;                        // 当前相位
    uint16_t sine_freq_hz;                      // 当前频率
    uint16_t sine_amplitude_mv;                 // 当前振幅（mVpp）

    // 脉冲波专用
    uint16_t pulse_high_code;                   // 高电平码值
    uint16_t pulse_low_code;                    // 低电平码值
    bool pulse_state;                           // 当前电平
} dac7311_handle_t;

/* ---- 生命周期 ---- */
dac7311_handle_t* dac7311_init(void);
void dac7311_deinit(dac7311_handle_t* handle);

/* ---- 低层写入 ---- */
int dac7311_write(dac7311_handle_t* handle, uint16_t value);          // 写 12 位原始值
int dac7311_write_voltage(dac7311_handle_t* handle,                   // 按电压写
                          uint16_t voltage_mv, uint16_t vref_mv);

/* ---- 直流模式 ---- */
int dac7311_set_dc(dac7311_handle_t* handle, uint16_t voltage_mv);   // 输出直流 0~2000mV

/* ---- 交流正弦模式 40Hz 2Vpp ---- */
int dac7311_start_sine(dac7311_handle_t* handle, uint16_t freq_hz,
                       uint16_t amplitude_mv);                       // 启动正弦波

/* ---- 脉冲模式 40Hz 50%占空比 ---- */
int dac7311_start_pulse(dac7311_handle_t* handle, uint16_t freq_hz,
                        uint16_t amplitude_mv);                      // 启动脉冲波

/* ---- 停止波形 ---- */
int dac7311_stop_waveform(dac7311_handle_t* handle);                 // 停止正弦/脉冲，保持当前输出

/* ---- 电源控制 ---- */
int dac7311_power_down(dac7311_handle_t* handle);
int dac7311_power_up(dac7311_handle_t* handle);

#endif /* __DAC7311_H__ */
