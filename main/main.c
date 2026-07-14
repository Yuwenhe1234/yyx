/* ============================================================
 * 模块1：头文件 — 引入所有依赖的库和驱动
 * ============================================================ */
#include <stdio.h>                          // 标准输入输出，提供 printf() 串口打印
#include <string.h>                          // 提供 strncpy、strcasecmp 等
#include <strings.h>                         // 提供 strcasecmp() 字符串比较
#include "led.h"                            // LED 驱动，控制板载 LED (GPIO2)
#include "key.h"                            // 按键驱动，检测 BOOT 按钮 (GPIO0)
#include "bluetooth.h"                      // 蓝牙 BLE 驱动，手机无线通信
#include "freertos/FreeRTOS.h"              // FreeRTOS 操作系统内核
#include "freertos/task.h"                  // FreeRTOS 任务管理，提供 vTaskDelay() 延时
#include "freertos/queue.h"                 // FreeRTOS 队列，用于 BLE 命令线程间传递
#include "ads1013.h"                        // I2C 外部 ADC 芯片驱动 (ADS1013, 地址 0x48)
#include "dac7311.h"                        // SPI 外部 DAC 芯片驱动 (DAC7311, 12位)
#include "esp_log.h"                        // ESP32 日志库，提供 ESP_LOGI/ESP_LOGE 等

/* ============================================================
 * 模块2：日志标签 — 本文件的日志前缀标识
 * ============================================================ */
static const char *TAG = "APP_MAIN";        // 所有日志打印时显示 [APP_MAIN]，方便串口调试时定位来源

/* ============================================================
 * 模块3：BLE → DAC 命令队列（解耦 BLE 线程和 DAC 操作线程）
 * ============================================================ */
#define CMD_QUEUE_LEN   8                   // 队列深度：最多缓存 8 条命令
static QueueHandle_t g_dac_cmd_queue = NULL;

/* ============================================================
 * 模块4：DAC 命令处理任务 — 独立线程，慢慢做 build_sine_lut 也不影响 BLE
 * ============================================================ */
static void dac_cmd_task(void *arg)
{
    dac7311_handle_t *dac = (dac7311_handle_t *)arg;
    char cmd_buf[64];

    while (1) {
        // 阻塞等待 BLE 层投递命令，超时不退出
        if (xQueueReceive(g_dac_cmd_queue, cmd_buf, portMAX_DELAY) != pdTRUE)
            continue;

        char ack[64];

        // ---- 直流模式：DC0~DC2.0（V）----
        float dc_voltage;
        if (sscanf(cmd_buf, "DC%f", &dc_voltage) == 1) {
            if (dc_voltage < 0.0f || dc_voltage > 2.0f) {
                bluetooth_notify_text("ERR: DC 0-2V only");
                continue;
            }
            uint16_t mv = (uint16_t)(dc_voltage * 1000.0f);
            dac7311_set_dc(dac, mv);
            snprintf(ack, sizeof(ack), "ACK DC %.2fV", dc_voltage);
            bluetooth_notify_text(ack);
            sensor_data_t s = { .dac_value = (uint16_t)(mv * 4096UL / DAC_VREF_MV),
                                .dac_mode = 0, .dac_param = mv, .dac_valid = true };
            bluetooth_send_sensor_data(&s);
            ESP_LOGI(TAG, "DAC: DC %.2fV", dc_voltage);
            continue;
        }

        // ---- 正弦模式：40Hz 2Vpp ----
        if (strcasecmp(cmd_buf, "SINE") == 0) {
            // ★ 这里在独立任务中执行，build_sine_lut 阻塞 3-10ms 不影响 BLE
            dac7311_start_sine(dac, 40, 2000);
            bluetooth_notify_text("ACK SINE 40Hz 2Vpp");
            sensor_data_t s = { .dac_mode = 1, .dac_param = 2000, .dac_valid = true };
            bluetooth_send_sensor_data(&s);
            ESP_LOGI(TAG, "DAC: SINE 40Hz 2Vpp");
            continue;
        }

        // ---- 脉冲模式：40Hz 50% 占空比 2Vpp ----
        if (strcasecmp(cmd_buf, "PLU") == 0) {
            dac7311_start_pulse(dac, 40, 2000);
            bluetooth_notify_text("ACK PLU 40Hz 2Vpp");
            sensor_data_t s = { .dac_mode = 2, .dac_param = 2000, .dac_valid = true };
            bluetooth_send_sensor_data(&s);
            ESP_LOGI(TAG, "DAC: PULSE 40Hz 2Vpp");
            continue;
        }
    }
}

/* ============================================================
 * 模块5：BLE 命令回调 — 在 NimBLE host_task 上下文中调用
 *         只做一件事：把命令扔进队列，立即返回（<10µs）
 * ============================================================ */
static void ble_command_handler(const char *cmd, void *ctx)
{
    // 非阻塞发送到 dac_cmd_task 处理
    if (g_dac_cmd_queue) {
        char cmd_copy[64];
        strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
        cmd_copy[sizeof(cmd_copy) - 1] = '\0';
        xQueueSend(g_dac_cmd_queue, cmd_copy, 0);
    }
}

/* ============================================================
 * 模块6：程序入口 — 芯片上电后自动调用
 * ============================================================ */
void app_main(void)
{
    /* ---------- 命令队列 + 独立任务 ---------- */
    g_dac_cmd_queue = xQueueCreate(CMD_QUEUE_LEN, 64);

    /* ---------- 基础外设初始化 ---------- */
    led_init();                             // 初始化 LED：配置 GPIO2 为输出模式，初始高阻态
    key_init();                             // 初始化按键：配置 GPIO0 为输入模式，启用内部上拉
    led_off();                              // 熄灭 LED，确保启动时灯是灭的
    bluetooth_init();                       // 初始化蓝牙 BLE 协议栈（NimBLE），开始广播等待手机连接
    printf("Hello world!\n");               // 串口打印启动信息，确认程序已正常运行

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
        dac7311_set_dc(dac, 0);             // 默认直流模式 0V

        // ★ 创建独立的 DAC 命令处理任务（解耦 BLE 线程）
        xTaskCreate(dac_cmd_task, "dac_cmd", 3072, dac, 5, NULL);
    }

    /* ---------- 主循环专用计数器 ---------- */
    uint16_t adc_count = 0;

    /* ---------- 主循环（无限循环，永不退出）---------- */
    while(1){
        sensor_data_t sensor = {0};

        /* --- 按键检测（每次循环都检查）--- */
        if(key_is_pressed()){
            led_toggle();
            (void)bluetooth_send_key_event(1);
        }

        /* --- I2C 读取外部 ADC（每 500ms 一次）--- */
        if (adc && adc_count % 5 == 0) {
            uint16_t raw_value;
            int16_t voltage_mv;

            if (ads1013_read_raw(adc, &raw_value) == 0) {
                voltage_mv = ads1013_raw_to_voltage_mv(raw_value, 4096);
                sensor.adc_raw = raw_value;
                sensor.adc_voltage_mv = voltage_mv;
                sensor.adc_valid = true;
                ESP_LOGI(TAG, "ADS1013 - Raw: %d, Voltage: %d mV", raw_value, voltage_mv);
            }
        }

        /* --- 推送传感器数据到蓝牙模块 --- */
        if (sensor.adc_valid) {
            bluetooth_send_sensor_data(&sensor);
        }

        adc_count++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* ---------- 清理代码（永远不会执行，仅为代码规范）---------- */
    if (adc) ads1013_deinit(adc);
    if (dac) dac7311_deinit(dac);
}
