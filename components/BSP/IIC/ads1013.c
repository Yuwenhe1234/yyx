/**
 * ============================================================
 * ADS1013 驱动 — 12 位 I2C ADC
 * ============================================================
 *
 * 【芯片简介】
 *   TI 公司 12 位模数转换器（ADC），I2C 接口，10 脚封装。
 *   特点：4 路单端或 2 路差分输入，可编程增益，内置参考电压。
 *   本项目配置：单端 AIN0 vs GND，连续转换，±4.096V 量程，1600 SPS。
 *
 * 【I2C 通信协议】
 *   - 2 线接口：SDA（数据）+ SCL（时钟），总线共享，从机地址区分
 *   - 速率：100 kHz 标准模式
 *   - 读/写都是主机（ESP32）发起，从机（ADS1013）响应
 *   - 每个寄存器 16 位，读写时先发寄存器地址再发/收数据
 *
 * 【I2C 读时序（数据手册 Fig 29）】
 *   ① 主机发 START
 *   ② 主机发 7 位地址 + W 位 = 0x90（0x48<<1 | 0）
 *   ③ 从机回 ACK
 *   ④ 主机发 1 字节寄存器指针（0x00 或 0x01）
 *   ⑤ 从机回 ACK
 *   ⑥ 主机发 Repeated START
 *   ⑦ 主机发 7 位地址 + R 位 = 0x91（0x48<<1 | 1）
 *   ⑧ 从机回 ACK
 *   ⑨ 从机发数据 MSB → 主机回 ACK
 *   ⑩ 从机发数据 LSB → 主机回 NAK
 *   ⑪ 主机发 STOP
 *
 * 【I2C 写时序（数据手册 Fig 28）】
 *   ① 主机发 START
 *   ② 主机发 7 位地址 + W 位 = 0x90
 *   ③ 从机回 ACK
 *   ④ 主机发 3 字节：寄存器指针 + 数据 MSB + 数据 LSB
 *   ⑤ 每字节从机回 ACK
 *   ⑥ 主机发 STOP
 *
 * 【转换结果格式】
 *   12 位数据左对齐在 16 位寄存器中：[D11 D10 ... D0 0 0 0 0]
 *   代码右移 4 位取出实际值：raw = (data[0]<<8 | data[1]) >> 4
 *   二进制补码格式：bit11=0 正数，bit11=1 负数
 *     正数：直接使用
 *     负数：signed = -(4096 - raw)
 *
 * 【代码结构】
 *   ads1013_init()           初始化：I2C 总线 → 挂设备 → 写配置
 *   ads1013_deinit()         释放：拔设备 → 释放总线 → 释放内存
 *   ads1013_read_raw()       读转换结果：I2C 读 0x00 寄存器，取出 12 位值
 *   ads1013_raw_to_voltage_mv() 纯计算：raw → 毫伏（不涉及 I2C）
 *   ads1013_write_config()   写配置寄存器
 *   ads1013_read_config()    读配置寄存器（调试校验用）
 * ============================================================ */

#include "ads1013.h"
#include "esp_log.h"
#include "esp_err.h"
#include <stdlib.h>

static const char *TAG = "ADS1013";                     // 日志标签

/* ============================================================
 * ads1013_init — 初始化 ADS1013
 * ============================================================
 * 四步：配 I2C0 总线 → 挂 ADS1013 设备 → 写配置寄存器 → 校验回读
 * 成功返回句柄指针，失败返回 NULL
 * ============================================================ */
ads1013_handle_t* ads1013_init(void)
{
    // 分配句柄内存
    ads1013_handle_t* handle = (ads1013_handle_t*)malloc(sizeof(ads1013_handle_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for ADS1013 handle");
        return NULL;
    }

    // ---- 第 1 步：初始化 I2C0 总线硬件 ----
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,              // 时钟源默认（SOC 内部 APB 时钟）
        .glitch_ignore_cnt = 7,                         // 毛刺过滤：短于 7 个时钟周期的脉冲被忽略
        .flags = {
            .enable_internal_pullup = true,             // 启用 ESP32 内部上拉（弱上拉 ~45kΩ）
        },
        .sda_io_num = ADS1013_SDA_GPIO,                // SDA → GPIO21 → ADS1013 第4脚
        .scl_io_num = ADS1013_SCL_GPIO,                // SCL → GPIO22 → ADS1013 第3脚
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &handle->bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // ---- 第 2 步：挂载 ADS1013 到 I2C 总线 ----
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,          // 7 位地址模式（I2C 标准）
        .device_address = ADS1013_I2C_ADDR,             // 0x48（ADDR 脚接地）
        .scl_speed_hz = ADS1013_I2C_FREQ,               // 100 kHz 标准模式
    };

    ret = i2c_master_bus_add_device(handle->bus_handle, &dev_config, &handle->dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(handle->bus_handle);         // 清理总线
        free(handle);                                   // 清理句柄
        return NULL;
    }

    // 上拉电阻提示
    ESP_LOGW(TAG, "Internal pull-ups enabled; external 2k-5k pull-up resistors are strongly "
                  "recommended for reliable I2C communication at 100 kHz");

    // ---- 第 3 步：写配置寄存器，启动连续转换 ----
    // 配置值 0x92C3 各位含义：
    //   bit15   OS=1    (连续模式下无实际作用)
    //   bit14:12 MUX=000 (AIN0 vs GND，单端)
    //   bit11:9 PGA=001  (±4.096V 量程)
    //   bit8   MODE=0   (连续转换模式)
    //   bit7:5 DR=110   (3300 次采样/秒，芯片最高速率)
    //   bit4:0 =000_11  (比较器禁用)
    uint16_t config = (1 << ADS1013_OS_SHIFT)      |    // bit15=1
                      (0 << ADS1013_MUX_SHIFT)     |    // bit14:12=000 → AIN0 vs GND（单端）
                      (1 << ADS1013_PGA_SHIFT)     |    // bit11:9=001 → ±4.096V
                      (0 << ADS1013_MODE_SHIFT)    |    // bit8=0 → 连续转换
                      (6 << ADS1013_DR_SHIFT)      |    // bit7:5=110 → 3300 SPS
                      (3 << ADS1013_COMP_QUE_SHIFT);    // bit1:0=11 → 禁用比较器

    ret = ads1013_write_config(handle, config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config, ADC may use wrong settings!");
        // 不直接退出，让调用者决定；至少日志里能看到错误
    }

    // ---- 第 4 步：回读校验（OS 位会不同，掩码掉再比较）----
    // OS 位在连续模式下：写入=1（无作用），读取=0（设备正在转换中）
    // 这是正常行为，所以校验时忽略 bit15
    uint16_t readback = 0;
    if (ads1013_read_config(handle, &readback) == ESP_OK) {
        uint16_t mask = ~(1 << ADS1013_OS_SHIFT);       // 掩码，忽略 OS 位
        if ((readback & mask) != (config & mask)) {      // 只比较 OS 以外的位
            ESP_LOGW(TAG, "Config mismatch: wrote 0x%04X, read back 0x%04X", config, readback);
        }
    } else {
        ESP_LOGW(TAG, "Could not read back config register; I2C may be unreliable");
    }

    ESP_LOGI(TAG, "ADS1013 initialized successfully");
    return handle;
}

/* ============================================================
 * ads1013_deinit — 释放所有资源
 * ============================================================ */
void ads1013_deinit(ads1013_handle_t* handle)
{
    if (!handle) return;

    i2c_master_bus_rm_device(handle->dev_handle);       // 从总线移除设备
    i2c_del_master_bus(handle->bus_handle);             // 释放 I2C0 总线硬件
    free(handle);                                       // 释放句柄内存

    ESP_LOGI(TAG, "ADS1013 deinitialized");
}

/* ============================================================
 * ads1013_write_config — 写配置寄存器（0x01）
 * ============================================================
 *
 * I2C 总线实际传输（ESP-IDF 驱动自动处理 START/STOP/ACK）：
 *   SCL  ______/‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾\______
 *   SDA  \START\  0x90 \ACK\ 0x01 \ACK\ MSB \ACK\ LSB \ACK\STOP
 *
 * ============================================================ */
int ads1013_write_config(ads1013_handle_t* handle, uint16_t config)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // 拼装 3 字节：寄存器地址 + 16 位数据（高位在前）
    uint8_t data[3];
    data[0] = ADS1013_REG_CONFIG;                       // 第 1 字节：寄存器地址 0x01
    data[1] = (config >> 8) & 0xFF;                     // 第 2 字节：配置高 8 位[15:8]
    data[2] = config & 0xFF;                            // 第 3 字节：配置低 8 位[7:0]

    // i2c_master_transmit 自动处理：
    //   START → 从机地址+W → 发 3 字节（每个等 ACK）→ STOP
    esp_err_t ret = i2c_master_transmit(handle->dev_handle, data, 3, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(ret));
    }
    return ret;
}

/* ============================================================
 * ads1013_read_config — 读配置寄存器（0x01）
 * ============================================================
 *
 * I2C 总线实际传输：
 *   主机发：START → 0x90(W) → 0x01 → Repeated START → 0x91(R)
 *   从机回：MSB → ACK → LSB → NAK → STOP
 * ============================================================ */
int ads1013_read_config(ads1013_handle_t* handle, uint16_t* config)
{
    if (!handle || !config) return ESP_ERR_INVALID_ARG;

    uint8_t reg = ADS1013_REG_CONFIG;                   // 要读的寄存器地址
    uint8_t data[2];                                    // 接收 2 字节缓冲区

    // i2c_master_transmit_receive 自动处理：
    //   START → 地址+W → 发 reg → Repeated START → 地址+R → 收 2 字节 → STOP
    esp_err_t ret = i2c_master_transmit_receive(handle->dev_handle, &reg, 1, data, 2, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read config: %s", esp_err_to_name(ret));
        return ret;
    }

    // 拼回 16 位值（高位在前）
    *config = ((uint16_t)data[0] << 8) | data[1];
    return ESP_OK;
}

/* ============================================================
 * ads1013_read_raw — 读转换结果寄存器（0x00），取出 12 位 ADC 值
 * ============================================================
 *
 * 【数据提取流程】
 *   ADS1013 转换结果格式（16 位寄存器）：
 *     data[0] = [D11 D10 D9 D8 D7 D6 D5 D4]   ← 高 8 位
 *     data[1] = [D3  D2  D1  D0  0  0  0  0]  ← 低 8 位（低 4 位恒 0）
 *
 *   ① 拼 16 位：raw16 = (data[0] << 8) | data[1]
 *      例：D11~D0=2047 → data[0]=0x7F, data[1]=0xF0 → raw16=0x7FF0
 *   ② 右移 4 位：raw = (raw16 >> 4) & 0xFFF
 *      例：0x7FF0 >> 4 = 0x07FF = 2047
 *
 *   结果范围：0 ~ 4095（12 位无符号）
 *   中心值 2048 对应 0V（差分输入时），本项目单端输入，值在 0~2047 为正
 * ============================================================ */
int ads1013_read_raw(ads1013_handle_t* handle, uint16_t* raw_value)
{
    if (!handle || !raw_value) return ESP_ERR_INVALID_ARG;

    uint8_t reg = ADS1013_REG_CONVERSION;               // 转换结果寄存器地址 0x00
    uint8_t data[2];                                    // 接收 2 字节

    // I2C 读写组合：先发寄存器地址 0x00，再读 2 字节
    esp_err_t ret = i2c_master_transmit_receive(handle->dev_handle, &reg, 1, data, 2, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read conversion: %s", esp_err_to_name(ret));
        return ret;
    }

    // 拆包：16 位寄存器 → 12 位实际值
    *raw_value = ((uint16_t)data[0] << 8) | data[1];    // 拼 16 位
    *raw_value = (*raw_value >> 4) & 0xFFF;              // 右移 4 位，掩码保留 12 位

    return ESP_OK;
}

/* ============================================================
 * ads1013_raw_to_voltage_mv — 原始值转毫伏（纯计算，不涉及 I2C）
 * ============================================================
 *
 * 【换算原理】
 *   ADS1013 输出二进制补码格式的 12 位值。
 *   PGA 决定满量程（FSR），如 ±2.048V 量程：
 *     正满量程：raw=2047 → +2047mV（注：满量程实际是 2048 而非 2047）
 *     零：      raw=0    → 0mV
 *     负满量程：raw=-2048 → -2048mV
 *
 * 【补码解码】
 *   bit11=0（raw ≤ 2047）：正数，直接使用
 *   bit11=1（raw ≥ 2048）：负数，signed = -(4096 - raw)
 *     例：raw=0xFFF(4095) → signed = -(4096-4095) = -1
 *     例：raw=0x800(2048) → signed = -(4096-2048) = -2048
 *
 * 【电压公式】
 *   对于 ±FSR 量程，1 LSB = FSR / 2048
 *   voltage_mv = signed_value × pga_range / 2048
 *
 *   例：signed_value=1024, pga_range=2048mV
 *       voltage = 1024 × 2048 / 2048 = 1024 mV
 * ============================================================ */
int16_t ads1013_raw_to_voltage_mv(uint16_t raw_value, uint16_t pga_range)
{
    // 二进制补码 → 有符号整数
    int16_t signed_value = (int16_t)raw_value;          // 先当正数处理
    if (raw_value & 0x800) {                            // bit11=1 → 负数
        signed_value = -(int16_t)(0x1000 - raw_value);  // 补码转原码：-(4096 - raw)
    }

    // 有符号值 × 量程 / 2048 → 毫伏
    // 用 int32_t 防溢出：int16 × uint16 可能超 int16 范围
    return (int16_t)(((int32_t)signed_value * (int32_t)pga_range) / 2048);
}
