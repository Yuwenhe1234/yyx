/* ============================================================
 * 模块1：头文件 — 引入所有依赖的库和驱动
 * ============================================================ */
#include <stdio.h>                          // 标准输入输出，提供 printf() 串口打印
#include <strings.h>                         // 提供 strcasecmp() 字符串比较
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
static void ble_command_handler(const char *cmd, void *ctx)  // cmd: 手机发来的文本命令，ctx: DAC 句柄
{
    dac7311_handle_t *dac = (dac7311_handle_t *)ctx;
    char ack[64];

    // ---- 直流模式：DC0~DC2.0（V）----
    float dc_voltage;
    if (sscanf(cmd, "DC%f", &dc_voltage) == 1) {
        if (dc_voltage < 0.0f || dc_voltage > 2.0f) {
            bluetooth_notify_text("ERR: DC 0-2V only");
            return;
        }
        uint16_t mv = (uint16_t)(dc_voltage * 1000.0f);
        dac7311_set_dc(dac, mv);
        snprintf(ack, sizeof(ack), "ACK DC %.2fV", dc_voltage);
        bluetooth_notify_text(ack);
        // 推送 DAC 状态给 BLE 流
        sensor_data_t s = { .dac_value = (uint16_t)(mv * 4096UL / DAC_VREF_MV),
                            .dac_mode = 0, .dac_param = mv, .dac_valid = true };
        bluetooth_send_sensor_data(&s);
        ESP_LOGI(TAG, "BLE: DC %.2fV", dc_voltage);
        return;
    }

    // ---- 正弦模式：40Hz 2Vpp ----
    if (strcasecmp(cmd, "SINE") == 0) {
        dac7311_start_sine(dac, 40, 2000);
        bluetooth_notify_text("ACK SINE 40Hz 2Vpp");
        sensor_data_t s = { .dac_mode = 1, .dac_param = 2000, .dac_valid = true };
        bluetooth_send_sensor_data(&s);
        ESP_LOGI(TAG, "BLE: SINE 40Hz 2Vpp");
        return;
    }

    // ---- 脉冲模式：40Hz 50% 占空比 2Vpp ----
    if (strcasecmp(cmd, "PLU") == 0) {
        dac7311_start_pulse(dac, 40, 2000);
        bluetooth_notify_text("ACK PLU 40Hz 2Vpp");
        sensor_data_t s = { .dac_mode = 2, .dac_param = 2000, .dac_valid = true };
        bluetooth_send_sensor_data(&s);
        ESP_LOGI(TAG, "BLE: PULSE 40Hz 2Vpp");
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
        bluetooth_register_command_callback(ble_command_handler, dac); // 注册 BLE 命令回调
        dac7311_set_dc(dac, 0);             // 默认直流模式 0V，手机发 DC/SINE/PLU 切换
    }

    /* ---------- 模块8：主循环专用计数器 ---------- */
    uint16_t adc_count = 0;                 // ADC 读取节奏计数器：每 5 次循环 (=500ms) 触发一次 ADC 读取

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
                voltage_mv = ads1013_raw_to_voltage_mv(raw_value, 4096); // 纯计算：raw → mV（PGA=±4.096V）
                sensor.adc_raw = raw_value;         // 填入传感器结构体：原始值
                sensor.adc_voltage_mv = voltage_mv; // 填入传感器结构体：电压值
                sensor.adc_valid = true;            // 标记 ADC 数据有效
                ESP_LOGI(TAG, "ADS1013 - Raw: %d, Voltage: %d mV", raw_value, voltage_mv); // 串口输出
            }
        }

        /* --- 子模块9.3：推送传感器数据到蓝牙模块 --- */
        if (sensor.adc_valid) {                     // 有 ADC 新数据就推送
            bluetooth_send_sensor_data(&sensor);    // 写入蓝牙缓存，由 streaming_task 通过 BLE 通知手机
        }

        adc_count++;                        // ADC 计数器 +1，用于计算 500ms 间隔
        vTaskDelay(pdMS_TO_TICKS(100));     // 延时 100 毫秒，让出 CPU 给蓝牙等其他任务
    }

    /* ---------- 模块9：清理代码（永远不会执行，仅为代码规范）---------- */
    if (adc) ads1013_deinit(adc);           // 释放 I2C 总线 + ADS1013 设备句柄
    if (dac) dac7311_deinit(dac);           // 释放 SPI 总线 + DAC7311 设备句柄
}
