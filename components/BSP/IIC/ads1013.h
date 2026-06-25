#ifndef __ADS1013_H__
#define __ADS1013_H__

#include "driver/i2c_master.h"

/* ============================================================
 * I2C 硬件引脚与参数定义
 * ============================================================
 * ESP32 通过 I2C0 与 ADS1013 通信，2 线连接：
 *   ESP32             ADS1013（10脚封装）
 *   GPIO21 SDA  ────→ 4脚 SDA   串行数据线（双向，半双工）
 *   GPIO22 SCL  ────→ 3脚 SCL   串行时钟线（ESP32 发出）
 *   GND        ────→ 2脚 ADDR  地址选择脚（接地=地址 0x48）
 * ============================================================ */
#define ADS1013_I2C_ADDR    0x48                // I2C 从机地址（ADDR 脚接地）
                                                //   ADDR→GND=0x48, ADDR→VDD=0x49
                                                //   ADDR→SDA=0x4A, ADDR→SCL=0x4B
#define ADS1013_I2C_PORT    I2C_NUM_0           // 使用 ESP32 的 I2C0 外设
#define ADS1013_SCL_GPIO    22                  // 时钟线引脚
#define ADS1013_SDA_GPIO    21                  // 数据线引脚
#define ADS1013_I2C_FREQ    100000              // I2C 时钟 100 kHz（标准模式）

/* ============================================================
 * ADS1013 内部寄存器地址
 * ============================================================
 * ADS1013 有 4 个 16 位寄存器，通过指针寄存器选择：
 *   0x00 = 转换结果寄存器（只读，最新一次 ADC 转换的数值）
 *   0x01 = 配置寄存器（读/写，控制 ADC 工作模式）
 *   0x02 = 低阈值寄存器（比较器用）
 *   0x03 = 高阈值寄存器（比较器用）
 * ============================================================ */
#define ADS1013_REG_CONVERSION  0x00            // 转换结果：16bit，12位数据左对齐在 [15:4]
#define ADS1013_REG_CONFIG      0x01            // 配置：控制 MUX、PGA、MODE、速率等
#define ADS1013_REG_LO_THRESH   0x02            // 低阈值（本项目不用比较器）
#define ADS1013_REG_HI_THRESH   0x03            // 高阈值（本项目不用比较器）

/* ============================================================
 * 配置寄存器（0x01）各位位置
 * ============================================================
 *
 *   15   14 13 12   11 10  9    8     7  6  5    4    3    2    1  0
 *  ┌────┬────────┬─────────┬──────┬──────────┬─────┬─────┬─────┬──────┐
 *  │ OS │  MUX   │   PGA   │ MODE │    DR    │CMP_M│CMP_P│CMP_L│CMP_Q │
 *  └────┴────────┴─────────┴──────┴──────────┴─────┴─────┴─────┴──────┘
 *
 * OS(bit15)：      操作状态 / 启动转换
 *                  写1=启动单次转换，读1=转换完成，读0=转换中
 * MUX(bit14:12)：  输入多路选择器
 *                  000=AIN0 vs GND, 001=AIN1 vs GND
 *                  010=AIN2 vs GND, 011=AIN3 vs GND
 * PGA(bit11:9)：   可编程增益 = 满量程范围（FSR）
 *                  000=±6.144V, 001=±4.096V, 010=±2.048V
 *                  011=±1.024V, 100=±0.512V, 101=±0.256V
 * MODE(bit8)：     工作模式
 *                  0=连续转换，1=单次转换（默认）
 * DR(bit7:5)：     数据速率（采样率 SPS）
 *                  000=128, 001=250, 010=490, 011=920
 *                  100=1600, 101=2400, 110=3300
 * CMP_M(bit4)：    比较器模式：0=传统，1=窗口
 * CMP_P(bit3)：    比较器极性：0=低有效，1=高有效
 * CMP_L(bit2)：    比较器锁存：0=非锁存，1=锁存
 * CMP_Q(bit1:0)：  比较器队列：11=禁用比较器
 * ============================================================ */
#define ADS1013_OS_SHIFT        15              // 操作状态位
#define ADS1013_MUX_SHIFT       12              // 输入通道选择位
#define ADS1013_PGA_SHIFT       9               // 增益选择位
#define ADS1013_MODE_SHIFT      8               // 工作模式位
#define ADS1013_DR_SHIFT        5               // 数据速率位
#define ADS1013_COMP_MODE_SHIFT 4               // 比较器模式位
#define ADS1013_COMP_POL_SHIFT  3               // 比较器极性位
#define ADS1013_COMP_LAT_SHIFT  2               // 比较器锁存位
#define ADS1013_COMP_QUE_SHIFT  0               // 比较器队列位

/* ---- 数据速率选项 ---- */
#define ADS1013_DR_128SPS   0x00                // 每秒 128 次采样
#define ADS1013_DR_250SPS   0x01                // 每秒 250 次采样
#define ADS1013_DR_490SPS   0x02                // 每秒 490 次采样
#define ADS1013_DR_920SPS   0x03                // 每秒 920 次采样
#define ADS1013_DR_1600SPS  0x04                // 每秒 1600 次采样（本项目选用）
#define ADS1013_DR_2400SPS  0x05                // 每秒 2400 次采样
#define ADS1013_DR_3300SPS  0x06                // 每秒 3300 次采样

/* ---- 增益（满量程范围）选项 ---- */
#define ADS1013_PGA_6144MV  0x00                // ±6.144V
#define ADS1013_PGA_4096MV  0x01                // ±4.096V
#define ADS1013_PGA_2048MV  0x02                // ±2.048V（本项目选用）
#define ADS1013_PGA_1024MV  0x03                // ±1.024V
#define ADS1013_PGA_512MV   0x04                // ±0.512V
#define ADS1013_PGA_256MV   0x05                // ±0.256V

/* ============================================================
 * 设备句柄 — 封装 I2C 总线句柄和设备句柄
 * ============================================================ */
typedef struct {
    i2c_master_bus_handle_t bus_handle;         // I2C 总线句柄（用于释放总线）
    i2c_master_dev_handle_t dev_handle;         // I2C 设备句柄（用于收发数据）
} ads1013_handle_t;

/* ---- 生命周期 ---- */
ads1013_handle_t* ads1013_init(void);           // 初始化 I2C 总线 + 配置 ADS1013，返回句柄
void ads1013_deinit(ads1013_handle_t* handle);  // 移除设备 + 释放总线 + 释放句柄

/* ---- 核心读取 ---- */
int ads1013_read_raw(ads1013_handle_t* handle, uint16_t* raw_value);  // 读 12 位原始值（0~4095）
int16_t ads1013_raw_to_voltage_mv(uint16_t raw_value, uint16_t pga_range); // 纯计算：raw → mV

/* ---- 寄存器操作 ---- */
int ads1013_write_config(ads1013_handle_t* handle, uint16_t config);  // 写配置寄存器
int ads1013_read_config(ads1013_handle_t* handle, uint16_t* config);  // 读配置寄存器（调试用）

#endif /* __ADS1013_H__ */
