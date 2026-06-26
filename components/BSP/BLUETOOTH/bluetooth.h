#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 传感器数据结构体：main.c 填充后推入蓝牙模块 ---------- */
typedef struct {
    uint16_t adc_raw;           // ADS1013 原始 ADC 值 (0-4095, 12位)
    int16_t  adc_voltage_mv;    // ADS1013 转换后的电压值 (毫伏, 可为负值)
    uint16_t dac_value;         // DAC 当前输出值 (0-4095)
    uint8_t  dac_mode;          // DAC 模式：0=DC, 1=SINE, 2=PULSE
    uint16_t dac_param;         // DAC 参数：DC=mV, SINE/PULSE=mVpp
    bool     adc_valid;         // true 表示本次 adc_raw/adc_voltage_mv 是新数据
    bool     dac_valid;         // true 表示本次 dac_value 是新数据
} sensor_data_t;

/* ---------- 手机命令回调：蓝牙收到文本命令后调用 ---------- */
typedef void (*bluetooth_command_cb_t)(const char *command, void *user_ctx);

/* ---------- 原有的 BLE 管理函数 ---------- */
esp_err_t bluetooth_init(void);                     // 初始化 NimBLE 协议栈，开始广播
esp_err_t bluetooth_send_key_event(uint8_t pressed); // 发送按键事件通知给手机
bool bluetooth_is_connected(void);                   // 查询是否有手机已连接
bool bluetooth_is_streaming_enabled(void);           // 查询手机是否已订阅通知

/* ---------- 新增：传感器数据推送 ---------- */
esp_err_t bluetooth_send_sensor_data(const sensor_data_t *data); // main.c 调用，推送最新传感器读数

/* ---------- 新增：手机命令回调注册 ---------- */
void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *user_ctx); // 注册命令处理回调

/* ---------- 新增：主动发送文本通知到手机 ---------- */
esp_err_t bluetooth_notify_text(const char *text);       // 主模块调用，主动推文本到手机（用于 ACK 等）

#ifdef __cplusplus
}
#endif
