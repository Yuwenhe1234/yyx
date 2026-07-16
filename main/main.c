/* ============================================================
 * main.c — BLE 无线刺激器主控
 * ADC: esp_timer 1250us ISR → Semaphore → 高优Task I2C = 800Hz
 * BLE: streaming_task 25ms → 紧凑二进制帧 (≤20采样点/帧)
 * 串口: 每 10ms 打印 ADC 数据到 ESP-IDF 监视器
 * ============================================================ */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "led.h"
#include "key.h"
#include "bluetooth.h"
#include "ads1013.h"
#include "dac7311.h"
#include "esp_log.h"

static const char *TAG = "APP_MAIN";
static ads1013_handle_t *g_adc = NULL;
static dac7311_handle_t *g_dac = NULL;

/* ── ADC 高速采样: esp_timer 1250us → Semaphore → Task ── */
static SemaphoreHandle_t g_sem = NULL;
static volatile bool g_sem_given = false;

static void adc_timer_cb(void *arg)
{
    (void)arg;
    if (!g_sem_given) {
        g_sem_given = true;
        BaseType_t w = pdFALSE;
        xSemaphoreGiveFromISR(g_sem, &w);
        if (w) portYIELD_FROM_ISR();
    }
}

static void adc_task(void *param)
{
    (void)param;
    while (1) {
        if (xSemaphoreTake(g_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_sem_given = false;
            if (g_adc) {
                uint16_t r; int16_t v;
                if (ads1013_read_raw(g_adc, &r) == 0) {
                    v = ads1013_raw_to_voltage_mv(r, 4096);
                    bluetooth_push_adc_sample(r, v);
                    /* ★ 串口输出，可在 idf.py monitor 或 putty/screen 中看到 */
                    printf("ADC raw=%u mv=%d\n", r, v);
                }
            }
        }
    }
}

/* ── BLE 命令回调 ── */
static void ble_cmd_cb(const char *cmd, void *ctx)
{
    dac7311_handle_t *dac = (dac7311_handle_t *)ctx;
    char ack[64];
    float v;
    if (sscanf(cmd, "DC%f", &v) == 1) {
        if (v < 0.0f || v > 2.0f) { bluetooth_notify_text("ERR: DC 0-2V only"); return; }
        uint16_t mv = (uint16_t)(v * 1000.0f);
        dac7311_set_dc(dac, mv);
        snprintf(ack, sizeof(ack), "ACK DC %.2fV", v);
        bluetooth_notify_text(ack);
        return;
    }
    if (!strcasecmp(cmd, "SINE")) {
        dac7311_start_sine(dac, 40, 2000);
        bluetooth_notify_text("ACK SINE 40Hz 2Vpp");
        return;
    }
    if (!strcasecmp(cmd, "PLU")) {
        dac7311_start_pulse(dac, 40, 2000);
        bluetooth_notify_text("ACK PLU 40Hz 2Vpp");
        return;
    }
    bluetooth_notify_text("ERR: Unknown cmd");
}

/* ── 入口 ── */
void app_main(void)
{
    led_init(); key_init(); led_off();
    bluetooth_init();
    ESP_LOGI(TAG, "Hello world!");

    g_adc = ads1013_init();
    if (!g_adc) ESP_LOGE(TAG, "ADS1013 fail");

    g_dac = dac7311_init();
    if (!g_dac) { ESP_LOGE(TAG, "DAC7311 fail"); }
    else {
        bluetooth_register_command_callback(ble_cmd_cb, g_dac);
        dac7311_set_dc(g_dac, 0);
    }

    /* 800Hz 硬件定时器 + 高优 I2C 采样 */
    g_sem = xSemaphoreCreateBinary();
    if (g_sem && g_adc) {
        xTaskCreatePinnedToCore(adc_task, "adc", 2048, NULL, 9, NULL, 0);
        const esp_timer_create_args_t ta = {
            .callback = adc_timer_cb,
            .name     = "adc_t"
        };
        esp_timer_handle_t ht = NULL;
        esp_timer_create(&ta, &ht);
        esp_timer_start_periodic(ht, 1250); // 800Hz → 20点/25ms
        ESP_LOGI(TAG, "ADC 800Hz timer ON");
    }

    /* 主循环: 按键 */
    while (1) {
        if (key_is_pressed()) { led_toggle(); bluetooth_send_key_event(1); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
