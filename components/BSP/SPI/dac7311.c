/**
 * ============================================================
 * DAC 驱动 — 12 位 SPI DAC（DAC7311/DAC7571 系列）
 * ============================================================
 *
 * 【架构】Core 1 独占 high-priority 任务，esp_timer_get_time() 定时，
 *   spi_device_polling_transmit 直写 SPI 寄存器。
 *   零 ISR、零中断、零信号量 → 完全不干扰 Core 0 上的 BLE。
 *
 *   精度：1µs 定时分辨率（用硬件 cycle counter 换算微秒）。
 *   任务优先级 configMAX_PRIORITIES 确保不被任何其他任务打断。
 *
 * 【SPI】16 位帧，MSB 先发，SPI Mode 0，10 MHz。
 */
#include "dac7311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "DAC7311";
static void build_sine_lut(dac7311_handle_t* handle, uint16_t amplitude_mv);

/* ---- 波形生成任务：Core 1 独占，esp_timer_get_time() 精准定时 + polling SPI ---- */
static void IRAM_ATTR waveform_task(void *arg)
{
    dac7311_handle_t *h = (dac7311_handle_t *)arg;
    int64_t next_tick_us = 0;
    int64_t period_us = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        period_us = h->waveform_period_us;
        next_tick_us = esp_timer_get_time();

        while (h->waveform_mode != DAC7311_MODE_NONE) {
            next_tick_us += period_us;

            switch (h->waveform_mode) {
            case DAC7311_MODE_SINE: {
                spi_transaction_t t = { .length = 16, .tx_buffer = h->sine_lut[h->sine_phase] };
                spi_device_polling_transmit(h->spi_handle, &t);
                h->sine_phase = (h->sine_phase + 1) % DAC7311_SINE_LUT_SIZE;
                break;
            }
            case DAC7311_MODE_PULSE: {
                uint16_t code = h->pulse_state ? h->pulse_high_code : h->pulse_low_code;
                uint16_t tx = (DAC7311_CMD_NORMAL << 14) | ((code & 0xFFF) << 2);
                spi_transaction_t t = { .flags = SPI_TRANS_USE_TXDATA, .length = 16 };
                t.tx_data[0] = (tx >> 8) & 0xFF;
                t.tx_data[1] = tx & 0xFF;
                spi_device_polling_transmit(h->spi_handle, &t);
                h->pulse_state = !h->pulse_state;
                break;
            }
            default:
                break;
            }

            // 精准忙等直到下一个采样点
            int64_t now;
            while ((now = esp_timer_get_time()) < next_tick_us) {
                // busy-wait on Core 1: 不打扰 Core 0 的 BLE
                if (next_tick_us - now > 50) {
                    // 如果还差很多时间（>50µs），释放 CPU 一小段时间
                    // 但 Core 1 上没有别的任务需要 CPU，所以直接用 NOP 忙等也可以
                    // 用 taskYIELD 反而可能让调度器空闲 → 短暂睡眠
                }
            }
        }
    }
    vTaskDelete(NULL);
}

/* ============================================================
 * dac7311_init
 * ============================================================ */
dac7311_handle_t* dac7311_init(void)
{
    dac7311_handle_t* handle = (dac7311_handle_t*)calloc(1, sizeof(dac7311_handle_t));
    if (!handle) { ESP_LOGE(TAG, "malloc fail"); return NULL; }

    spi_bus_config_t bus = {
        .mosi_io_num = DAC7311_MOSI_GPIO, .miso_io_num = DAC7311_MISO_GPIO,
        .sclk_io_num = DAC7311_SCLK_GPIO, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };
    esp_err_t ret = spi_bus_initialize(DAC7311_SPI_HOST, &bus, 0);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SPI bus fail"); free(handle); return NULL; }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = DAC7311_SPI_FREQ, .mode = 0,
        .spics_io_num = DAC7311_CS_GPIO, .queue_size = 1, .flags = SPI_DEVICE_NO_DUMMY,
    };
    ret = spi_bus_add_device(DAC7311_SPI_HOST, &dev, &handle->spi_handle);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SPI dev fail"); spi_bus_free(DAC7311_SPI_HOST); free(handle); return NULL; }

    // 创建 Core 1 独占任务：最高优先级
    BaseType_t created = xTaskCreatePinnedToCore(waveform_task, "dac_wave", 2048, handle,
                                     configMAX_PRIORITIES, &handle->waveform_task, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Task create fail");
        spi_bus_free(DAC7311_SPI_HOST);
        free(handle);
        return NULL;
    }

    vTaskSuspend(handle->waveform_task); // 初始暂停，等 start_sine/start_pulse 唤醒

    // 预建 2000mVpp LUT
    build_sine_lut(handle, 2000);

    dac7311_write(handle, 0);
    ESP_LOGI(TAG, "DAC ready (SPI2 10MHz, Core1 no ISR)");
    return handle;
}

void dac7311_deinit(dac7311_handle_t* handle)
{
    if (!handle) return;
    dac7311_stop_waveform(handle);
    spi_bus_remove_device(handle->spi_handle);
    spi_bus_free(DAC7311_SPI_HOST);
    free(handle);
}

/* ============================================================
 * dac7311_write — 低频调用用 polling
 * ============================================================ */
int dac7311_write(dac7311_handle_t* handle, uint16_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (value > DAC7311_MAX_VALUE) value = DAC7311_MAX_VALUE;
    uint16_t tx = (DAC7311_CMD_NORMAL << 14) | ((value & 0xFFF) << 2);
    spi_transaction_t t = { .flags = SPI_TRANS_USE_TXDATA, .length = 16 };
    t.tx_data[0] = (tx >> 8) & 0xFF;
    t.tx_data[1] = tx & 0xFF;
    return spi_device_polling_transmit(handle->spi_handle, &t);
}

int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (vref_mv == 0) vref_mv = DAC_VREF_MV;
    uint32_t code = ((uint32_t)voltage_mv * 4096UL) / vref_mv;
    if (code > DAC7311_MAX_VALUE) code = DAC7311_MAX_VALUE;
    return dac7311_write(handle, (uint16_t)code);
}

int dac7311_set_dc(dac7311_handle_t* handle, uint16_t voltage_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (voltage_mv > DAC_VREF_MV) voltage_mv = DAC_VREF_MV;
    dac7311_stop_waveform(handle);
    uint32_t code = ((uint32_t)voltage_mv * 4096UL) / DAC_VREF_MV;
    if (code > DAC7311_MAX_VALUE) code = DAC7311_MAX_VALUE;
    return dac7311_write(handle, (uint16_t)code);
}

/* ============================================================
 * build_sine_lut — 512 点正弦表预格式化
 * ============================================================ */
static void build_sine_lut(dac7311_handle_t* handle, uint16_t amplitude_mv)
{
    uint16_t half = amplitude_mv / 2;
    uint16_t center = (uint16_t)(((uint32_t)half * 4096UL) / DAC_VREF_MV);
    for (int i = 0; i < DAC7311_SINE_LUT_SIZE; i++) {
        float a = 2.0f * (float)M_PI * (float)i / DAC7311_SINE_LUT_SIZE;
        int32_t c = (int32_t)center + (int32_t)((float)center * sinf(a));
        if (c < 0) c = 0;
        if (c > 4095) c = 4095;
        uint16_t tx = (DAC7311_CMD_NORMAL << 14) | (((uint16_t)c & 0xFFF) << 2);
        handle->sine_lut[i][0] = (tx >> 8) & 0xFF;
        handle->sine_lut[i][1] = tx & 0xFF;
    }
    handle->sine_amplitude_mv = amplitude_mv;
}

/* ---- start_sine：O(1)，通知 Core 1 任务开始 + 唤醒任务 ---- */
int dac7311_start_sine(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);

    // 振幅变化才重建 LUT
    if (handle->sine_amplitude_mv != amplitude_mv)
        build_sine_lut(handle, amplitude_mv);

    handle->sine_freq_hz = freq_hz;
    handle->sine_phase = 0;
    handle->waveform_period_us = 1000000ULL / ((uint32_t)freq_hz * DAC7311_SINE_LUT_SIZE);
    handle->waveform_mode = DAC7311_MODE_SINE;

    xTaskNotifyGive(handle->waveform_task); // 唤醒 Core 1 任务
    return ESP_OK;
}

int dac7311_start_pulse(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);
    uint16_t hi = (uint16_t)(((uint32_t)amplitude_mv * 4096UL) / DAC_VREF_MV);
    handle->pulse_high_code = hi > 4095 ? 4095 : hi;
    handle->pulse_low_code = 0;
    handle->pulse_state = true;
    handle->waveform_period_us = 1000000ULL / ((uint32_t)freq_hz * 2);
    handle->waveform_mode = DAC7311_MODE_PULSE;

    xTaskNotifyGive(handle->waveform_task);
    return ESP_OK;
}

int dac7311_stop_waveform(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->waveform_mode == DAC7311_MODE_NONE) return ESP_OK;
    handle->waveform_mode = DAC7311_MODE_NONE;
    // 任务内循环检测到 MODE_NONE 后会阻塞在 ulTaskNotifyTake
    return ESP_OK;
}

int dac7311_power_down(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);
    uint16_t tx = (DAC7311_CMD_POWERDOWN << 14);
    spi_transaction_t t = { .flags = SPI_TRANS_USE_TXDATA, .length = 16 };
    t.tx_data[0] = (tx >> 8) & 0xFF; t.tx_data[1] = tx & 0xFF;
    return spi_device_polling_transmit(handle->spi_handle, &t);
}

int dac7311_power_up(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return dac7311_write(handle, 0);
}
