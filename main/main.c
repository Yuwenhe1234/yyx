/* ============================================================
 * 模块1：头文件 — 引入所有依赖的库和驱动
 * ============================================================ */
#include <stdio.h>                          // 标准输入输出，提供 printf() 串口打印
#include <string.h>                         // 提供 strncpy 等
#include <strings.h>                        // 提供 strcasecmp() 字符串比较
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
static const char *TAG = "APP_MAIN";

/* ============================================================
 * 模块3：BLE 命令回调 — 手机发送 DC/SINE/PLU 命令时被蓝牙模块调用
 *         GPTimer ISR 在 Core 1 运行，不干扰 BLE，safe to call directly.
 * ============================================================ */
static void ble_command_handler(const char *cmd, void *ctx)
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
        ESP_LOGI(TAG, "DAC: DC %.2fV", dc_voltage);
        return;
    }

    // ---- 正弦模式：40Hz 2Vpp ----
    if (strcasecmp(cmd, "SINE") == 0) {
        // Core 1 GPTimer ISR 跑 SPI，不干扰 BLE
        dac7311_start_sine(dac, 40, 2000);
        bluetooth_notify_text("ACK SINE 40Hz 2Vpp");
        ESP_LOGI(TAG, "DAC: SINE 40Hz 2Vpp");
        return;
    }

    // ---- 脉冲模式：40Hz 50% 占空比 2Vpp ----
    if (strcasecmp(cmd, "PLU") == 0) {
        dac7311_start_pulse(dac, 40, 2000);
        bluetooth_notify_text("ACK PLU 40Hz 2Vpp");
        ESP_LOGI(TAG, "DAC: PULSE 40Hz 2Vpp");
        return;
    }

    bluetooth_notify_text("ERR: Unknown cmd");
}

/* ============================================================
 * 模块4：程序入口 — 芯片上电后自动调用
 * ============================================================ */
void app_main(void)
{
    /* ---------- 基础外设初始化 ---------- */
    led_init();
    key_init();
    led_off();
    bluetooth_init();
    printf("Hello world!\n");

    /* ---------- I2C 外部 ADC 初始化 ---------- */
    ESP_LOGI(TAG, "Initializing ADS1013 I2C ADC...");
    ads1013_handle_t* adc = ads1013_init();
    if (!adc) {
        ESP_LOGE(TAG, "Failed to initialize ADS1013");
    } else {
        ESP_LOGI(TAG, "ADS1013 initialized successfully");
    }

    /* ---------- SPI 外部 DAC 初始化 ---------- */
    ESP_LOGI(TAG, "Initializing DAC7311 SPI DAC...");
    dac7311_handle_t* dac = dac7311_init();
    if (!dac) {
        ESP_LOGE(TAG, "Failed to initialize DAC7311");
    } else {
        ESP_LOGI(TAG, "DAC7311 initialized successfully");
        bluetooth_register_command_callback(ble_command_handler, dac);
        dac7311_set_dc(dac, 0);
    }

    /* ---------- 主循环 ---------- */

    while(1){
        sensor_data_t sensor = {0};

        /* --- 按键检测 --- */
        if(key_is_pressed()){
            led_toggle();
            (void)bluetooth_send_key_event(1);
        }

        /* --- I2C 读取外部 ADC（每次循环都读）--- */
        if (adc) {
            uint16_t raw_value;
            int16_t voltage_mv;

            if (ads1013_read_raw(adc, &raw_value) == 0) {
                voltage_mv = ads1013_raw_to_voltage_mv(raw_value, 4096);
                sensor.adc_raw = raw_value;
                sensor.adc_voltage_mv = voltage_mv;
                sensor.adc_valid = true;
            }
        }

        if (sensor.adc_valid) {
            bluetooth_send_sensor_data(&sensor);
        }

        vTaskDelay(1);                      // 1 tick ≈ 10ms → 100 SPS
    }

    if (adc) ads1013_deinit(adc);
    if (dac) dac7311_deinit(dac);
}
