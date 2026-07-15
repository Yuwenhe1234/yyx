/**
 * ============================================================
 * DAC 驱动 — 12 位 SPI DAC（DAC7311/DAC7571 系列）
 * ============================================================
 *
 * 【架构】成熟方案：Core 1 GPTimer ISR 直接 polling SPI
 *   - GPTimer 由 Core 1 任务创建/注册回调 → ISR 自动绑定 Core 1
 *   - ISR 内直接 spi_device_polling_transmit（IDF 官方 API 允许，有 SPI_MASTER_ISR_ATTR 标记）
 *   - Core 0 完全不受影响 → BLE 100% 稳定
 *
 * 【SPI】16 位帧，MSB 先发，SPI Mode 0，10 MHz。
 */
#include "dac7311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "DAC7311";
static void build_sine_lut(dac7311_handle_t* handle, uint16_t amplitude_mv);

/* ---- GPTimer ISR：跑在 Core 1，直接 polling SPI（不占用 Core 0 中断）---- */
static bool IRAM_ATTR waveform_timer_isr(gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata, void *arg)
{
    dac7311_handle_t *h = (dac7311_handle_t *)arg;

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
    return false; // 不需要 yield
}

/* ---- 波形管理任务：跑在 Core 1，负责创建 GPTimer（ISR 自动绑定 Core 1）---- */
static void waveform_mgr_task(void *arg)
{
    dac7311_handle_t *h = (dac7311_handle_t *)arg;

    while (1) {
        // 等待命令
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);

        if (h->waveform_mode == DAC7311_MODE_NONE) continue;

        uint64_t period_us = h->waveform_period_us;

        // ★ 在 Core 1 上创建 GPTimer → ISR 自动分配在 Core 1
        gptimer_config_t cfg = { .clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                 .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
        if (gptimer_new_timer(&cfg, &h->waveform_timer) != ESP_OK) {
            ESP_LOGE(TAG, "GPTimer create fail");
            continue;
        }

        gptimer_alarm_config_t a = { .alarm_count = period_us, .reload_count = 0,
                                     .flags.auto_reload_on_alarm = true };
        gptimer_set_alarm_action(h->waveform_timer, &a);
        gptimer_event_callbacks_t cb = { .on_alarm = waveform_timer_isr };
        gptimer_register_event_callbacks(h->waveform_timer, &cb, h);
        gptimer_enable(h->waveform_timer);
        gptimer_start(h->waveform_timer);

        ESP_LOGI(TAG, "Waveform running (Core 1 ISR, %llu us)", period_us);

        // 阻塞直到收到 stop 或 mode 变化通知
        while (h->waveform_mode != DAC7311_MODE_NONE) {
            uint32_t ret = ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(100));
            if (ret > 0) break; // 收到新命令 → 重新配置
        }

        // 停止定时器
        if (h->waveform_timer) {
            gptimer_stop(h->waveform_timer);
            gptimer_disable(h->waveform_timer);
            gptimer_del_timer(h->waveform_timer);
            h->waveform_timer = NULL;
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

    // ★ Core 1 上创建管理任务（GPTimer 也在 Core 1 上创建 → ISR 绑定 Core 1）
    BaseType_t created = xTaskCreatePinnedToCore(waveform_mgr_task, "dac_mgr", 2048, handle,
                                     5, &handle->waveform_task, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Task create fail");
        spi_bus_free(DAC7311_SPI_HOST);
        free(handle);
        return NULL;
    }

    // 预建 2000mVpp LUT
    build_sine_lut(handle, 2000);

    dac7311_write(handle, 0);
    ESP_LOGI(TAG, "DAC ready (SPI2 10MHz, Core1 GPTimer ISR)");
    return handle;
}

void dac7311_deinit(dac7311_handle_t* handle)
{
    if (!handle) return;
    dac7311_stop_waveform(handle);
    if (handle->waveform_task) {
        vTaskDelete(handle->waveform_task);
        handle->waveform_task = NULL;
    }
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

/* ---- start_sine ---- */
int dac7311_start_sine(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);

    if (handle->sine_amplitude_mv != amplitude_mv)
        build_sine_lut(handle, amplitude_mv);

    handle->sine_freq_hz = freq_hz;
    handle->sine_phase = 0;
    handle->waveform_period_us = 1000000ULL / ((uint32_t)freq_hz * DAC7311_SINE_LUT_SIZE);
    handle->waveform_mode = DAC7311_MODE_SINE;

    xTaskNotifyGive(handle->waveform_task); // 通知 Core 1 管理任务
    return ESP_OK;
}

/* ---- start_pulse ---- */
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

/* ---- stop_waveform ---- */
int dac7311_stop_waveform(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->waveform_mode == DAC7311_MODE_NONE) return ESP_OK;

    // 告知管理任务停止
    handle->waveform_mode = DAC7311_MODE_NONE;
    // 管理任务在 100ms 超时内会检测到 MODE_NONE 并停定时器
    xTaskNotifyGive(handle->waveform_task);
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
