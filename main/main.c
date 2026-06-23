/* ============================================================
 * 模块1：头文件 — 引入所有依赖的库和驱动
 * ============================================================ */
#include <stdio.h>                          // 标准输入输出，提供 printf() 串口打印
#include "led.h"                            // LED 驱动，控制板载 LED (GPIO2)
#include "key.h"                            // 按键驱动，检测 BOOT 按钮 (GPIO0)
#include "bluetooth.h"                      // 蓝牙 BLE 驱动，手机无线通信
#include "freertos/FreeRTOS.h"              // FreeRTOS 操作系统内核
#include "freertos/task.h"                  // FreeRTOS 任务管理，提供 vTaskDelay() 延时
#include "ads1013.h"                        // I2C 外部 ADC 芯片驱动 (ADS1013, 地址 0x48)
#include "dac7311.h"                        // SPI 外部 DAC 芯片驱动 (DAC7311, 12位)
#include "esp_log.h"                        // ESP32 日志库，提供 ESP_LOGI/ESP_LOGE 等

/* ============================================================
 * 模块2：日志标签 — 本文件的日志前缀标识
 * ============================================================ */
static const char *TAG = "APP_MAIN";        // 所有日志打印时显示 [APP_MAIN]，方便串口调试时定位来源

/* ============================================================
 * 模块3：BLE 命令回调 — 手机发送 DC/SINE/PLU 命令时被蓝牙模块调用
 * ============================================================ */
static void ble_command_handler(const char *cmd, void *ctx)  // cmd: 手机发来的文本命令，ctx: DAC7311 句柄
{
    dac7311_handle_t *dac = (dac7311_handle_t *)ctx;         // 从上下文取出 DAC7311 句柄

    float dc_voltage;
    if (sscanf(cmd, "DC%f", &dc_voltage) == 1) {            // 解析 "DC1.5" 格式的电压设置命令
        if (dc_voltage < 0 || dc_voltage > 5.0f) {           // DAC7311 支持 0~5V（取决于 VREF）
            ESP_LOGW(TAG, "DC voltage %.2fV out of range", dc_voltage); // 超出范围警告
            return;
        }
        uint16_t mv = (uint16_t)(dc_voltage * 1000.0f);      // 电压转为毫伏
        dac7311_write_voltage(dac, mv, 5000);                // 写 DAC7311（参考电压 5000mV）
        ESP_LOGI(TAG, "BLE cmd: DC%.2fV → DAC7311 %u mV", dc_voltage, mv);
        return;
    }

    if (strcasecmp(cmd, "SINE") == 0) {                      // 手机发送 "SINE" 命令
        ESP_LOGW(TAG, "SINE waveform not yet implemented on external DAC");
        return;
    }

    if (strcasecmp(cmd, "PLU") == 0) {                       // 手机发送 "PLU" 命令
        ESP_LOGW(TAG, "PULSE waveform not yet implemented on external DAC");
        return;
    }
}

/* ============================================================
 * 模块4：程序入口 — 芯片上电后自动调用
 * ============================================================ */
void app_main(void)
{
    /* ---------- 模块4：基础外设初始化 ---------- */
    led_init();                             // 初始化 LED：配置 GPIO2 为输出模式，初始高阻态
    key_init();                             // 初始化按键：配置 GPIO0 为输入模式，启用内部上拉
    led_off();                              // 熄灭 LED，确保启动时灯是灭的
    bluetooth_init();                       // 初始化蓝牙 BLE 协议栈（NimBLE），开始广播等待手机连接
    printf("Hello world!\n");               // 串口打印启动信息，确认程序已正常运行

    /* ---------- 模块5：I2C 外部 ADC 初始化 ---------- */
    ESP_LOGI(TAG, "Initializing ADS1013 I2C ADC...");  // 日志：正在初始化 ADS1013
    ads1013_handle_t* adc = ads1013_init(); // 调用初始化函数：配置 I2C0 (SDA=21,SCL=22)，设置连续转换模式
    if (!adc) {                             // 如果返回 NULL，说明初始化失败
        ESP_LOGE(TAG, "Failed to initialize ADS1013");  // 打印错误日志
    } else {                                // 初始化成功
        ESP_LOGI(TAG, "ADS1013 initialized successfully"); // 打印成功日志
    }

    /* ---------- 模块7：SPI 外部 DAC 初始化 ---------- */
    ESP_LOGI(TAG, "Initializing DAC7311 SPI DAC...");    // 日志：正在初始化 DAC7311
    dac7311_handle_t* dac = dac7311_init(); // 调用初始化函数：配置 SPI2 (SCLK=18,MOSI=23,CS=5)，Mode 1
    if (!dac) {                             // 如果返回 NULL，说明初始化失败
        ESP_LOGE(TAG, "Failed to initialize DAC7311");   // 打印错误日志
    } else {                                // 初始化成功
        ESP_LOGI(TAG, "DAC7311 initialized successfully"); // 打印成功日志
        bluetooth_register_command_callback(ble_command_handler, dac); // 注册 BLE 命令回调：手机发 DC/SINE/PLU 时操作 DAC7311
    }

    /* ---------- 模块8：主循环专用计数器 ---------- */
    uint16_t adc_count = 0;                 // ADC 读取节奏计数器：每 5 次循环 (=500ms) 触发一次 ADC 读取
    uint16_t dac_count = 0;                 // DAC 波形步进计数器：每次循环 +1，用于生成锯齿波

    /* ---------- 模块9：主循环（无限循环，永不退出）---------- */
    while(1){
        sensor_data_t sensor = {0};         // 本次循环的传感器数据，填充后推入蓝牙模块

        /* --- 子模块9.1：按键检测（每次循环都检查）--- */
        if(key_is_pressed()){               // 检测 BOOT 按钮是否被按下（软件消抖后的结果）
            led_toggle();                   // 翻转 LED：亮 → 灭，灭 → 亮
            (void)bluetooth_send_key_event(1); // 通过 BLE 无线通知手机 "按键被按下"
        }

        /* --- 子模块9.2：I2C 读取外部 ADC（每 500ms 一次）--- */
        if (adc && adc_count % 5 == 0) {    // adc 非 NULL（初始化成功）且计数器是 5 的倍数（=500ms）
            uint16_t raw_value;             // 存放 ADS1013 原始读数（12 位，范围 0~4095）
            int16_t voltage_mv;             // 存放转换后的电压值（单位：毫伏，可为负值）

            if (ads1013_read_raw(adc, &raw_value) == 0) { // I2C 读取转换寄存器，返回 0 表示成功
                if (ads1013_read_voltage(adc, &voltage_mv, 2048) == 0) { // 将原始值转为毫伏（PGA=±2.048V）
                    sensor.adc_raw = raw_value;         // 填入传感器结构体：原始值
                    sensor.adc_voltage_mv = voltage_mv; // 填入传感器结构体：电压值
                    sensor.adc_valid = true;            // 标记 ADC 数据有效
                    ESP_LOGI(TAG, "ADS1013 - Raw: %d, Voltage: %d mV", raw_value, voltage_mv); // 串口输出
                }
            }
        }

        /* --- 子模块9.3：SPI 写入外部 DAC（每 100ms 一次，生成锯齿波）--- */
        if (dac) {                          // dac 非 NULL（初始化成功）才执行
            uint16_t dac_value = (dac_count * 64) & 0xFFF; // 锯齿波算法：dac_count×64 取低12位，0→64→128→…→4095→0→循环
            if (dac7311_write(dac, dac_value) == 0) { // SPI 发送 16 位数据到 DAC7311，返回 0 表示成功
                sensor.dac_value = dac_value;   // 填入传感器结构体：DAC 值
                sensor.dac_valid = true;        // 标记 DAC 数据有效
                if (dac_count % 10 == 0) {  // 每 10 次（=每 1 秒）打印一次，避免串口刷屏
                    ESP_LOGI(TAG, "DAC7311 - Value: %d", dac_value); // 串口输出当前 DAC 值
                }
            }
            dac_count++;                    // DAC 步进计数器 +1，驱动锯齿波前进
        }

        /* --- 子模块9.4：推送传感器数据到蓝牙模块 --- */
        if (sensor.adc_valid || sensor.dac_valid) { // 只要有新数据就推送
            bluetooth_send_sensor_data(&sensor);    // 写入蓝牙缓存，由 streaming_task 通过 BLE 通知手机
        }

        adc_count++;                        // ADC 计数器 +1，用于计算 500ms 间隔
        vTaskDelay(pdMS_TO_TICKS(100));     // 延时 100 毫秒，让出 CPU 给蓝牙等其他任务
    }

    /* ---------- 模块9：清理代码（永远不会执行，仅为代码规范）---------- */
    if (adc) ads1013_deinit(adc);           // 释放 I2C 总线 + ADS1013 设备句柄
    if (dac) dac7311_deinit(dac);           // 释放 SPI 总线 + DAC7311 设备句柄
}
