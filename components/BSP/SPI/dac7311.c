/**
 * ============================================================
 * DAC 驱动 — 12 位 SPI DAC（DAC7311/DAC7571 系列）
 * ============================================================
 *
 * 【三个输出模式】
 *   直流(DC)：输出固定电压 0~2V，BLE 发 "DC1.5" 设定
 *   正弦(SINE)：40Hz 2Vpp，256 点 LUT + 硬件定时器，BLE 发 "SINE" 启动
 *   脉冲(PULSE)：40Hz 50%占空比 2Vpp，定时器翻转电平，BLE 发 "PLU" 启动
 *
 * 【SPI 协议】
 *   16 位帧，MSB 先发，SPI Mode 1（CPOL=0,CPHA=1 下降沿采样）
 *   DB[15:14]=PD, DB[13:12]=X, DB[11:0]=DAC 数据
 *
 * 【输出公式（DAC7571 6脚SC70，VDD=3.3V）】
 *   Vout = VDD × CODE / 4096，1 code ≈ 0.806mV
 * ============================================================ */

#include "dac7311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "DAC7311";

/* ---- 前向声明 ---- */
static void waveform_task(void *arg);
static void build_sine_lut(dac7311_handle_t* handle, uint16_t amplitude_mv);

/* ============================================================
 * dac7311_init — 初始化 SPI 总线 + DAC + 正弦 LUT
 * ============================================================ */
dac7311_handle_t* dac7311_init(void)
{
    dac7311_handle_t* handle = (dac7311_handle_t*)calloc(1, sizeof(dac7311_handle_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory");
        return NULL;
    }

    // 初始化 SPI2 总线
    spi_bus_config_t bus_config = {
        .mosi_io_num = DAC7311_MOSI_GPIO,
        .miso_io_num = DAC7311_MISO_GPIO,
        .sclk_io_num = DAC7311_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
    };

    // 只发 2 字节，不用 DMA，中断模式更可靠
    esp_err_t ret = spi_bus_initialize(DAC7311_SPI_HOST, &bus_config, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // 挂载 DAC 设备
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = DAC7311_SPI_FREQ,
        .mode = 0,                          // CPOL=0, CPHA=0 → 上升沿采样
        .spics_io_num = DAC7311_CS_GPIO,
        .queue_size = 1,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ret = spi_bus_add_device(DAC7311_SPI_HOST, &dev_config, &handle->spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        spi_bus_free(DAC7311_SPI_HOST);
        free(handle);
        return NULL;
    }

    // 初始化输出为 0V
    dac7311_write(handle, 0);

    ESP_LOGI(TAG, "DAC initialized (SPI2, Mode 0, 10 MHz, GPTimer+Task)");
    return handle;
}

/* ============================================================
 * dac7311_deinit
 * ============================================================ */
void dac7311_deinit(dac7311_handle_t* handle)
{
    if (!handle) return;
    dac7311_stop_waveform(handle);              // 停定时器
    spi_bus_remove_device(handle->spi_handle);
    spi_bus_free(DAC7311_SPI_HOST);
    free(handle);
    ESP_LOGI(TAG, "DAC deinitialized");
}

/* ============================================================
 * dac7311_write — 写 12 位原始值到 DAC
 * ============================================================ */
int dac7311_write(dac7311_handle_t* handle, uint16_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (value > DAC7311_MAX_VALUE) value = DAC7311_MAX_VALUE;

    // SC70-6 封装：12 位数据在 DB[13:2]，左移 2 位
    uint16_t tx = (DAC7311_CMD_NORMAL << 14) | ((value & 0xFFF) << 2);

    spi_transaction_t trans = {
        .flags = SPI_TRANS_USE_TXDATA,
        .length = 16,
    };
    trans.tx_data[0] = (tx >> 8) & 0xFF;    // DB[15:8] 先发
    trans.tx_data[1] = tx & 0xFF;           // DB[7:0]  后发
    return spi_device_transmit(handle->spi_handle, &trans);
}

/* ============================================================
 * dac7311_write_voltage — 按电压写 DAC
 * ============================================================ */
int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (vref_mv == 0) vref_mv = DAC_VREF_MV;
    uint32_t code = ((uint32_t)voltage_mv * 4096UL) / vref_mv;
    if (code > DAC7311_MAX_VALUE) code = DAC7311_MAX_VALUE;
    return dac7311_write(handle, (uint16_t)code);
}

/* ============================================================
 * dac7311_set_dc — 直流模式，输出固定电压（0~VREF_mV）
 * ============================================================
 * DAC7311 2x 增益：Vout = 2 × VREF × CODE/4096 → CODE = mV × 2048 / VREF_mV
 * 例：VREF=825mV, 要 500mV → CODE = 500×4096/825 = 2482
 * ============================================================ */
int dac7311_set_dc(dac7311_handle_t* handle, uint16_t voltage_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (voltage_mv > DAC_VREF_MV) voltage_mv = DAC_VREF_MV;

    dac7311_stop_waveform(handle);              // 停波形
    // 使用 4096 除数直接映射（1 code = VREF/4096 V，不假设 2x 增益）
    uint32_t code = ((uint32_t)voltage_mv * 4096UL) / DAC_VREF_MV;
    if (code > DAC7311_MAX_VALUE) code = DAC7311_MAX_VALUE;
    int ret = dac7311_write(handle, (uint16_t)code);
    ESP_LOGI(TAG, "DC %u mV → code %lu", voltage_mv, (unsigned long)code);
    return ret;
}

/* ============================================================
 * build_sine_lut — 按振幅重建正弦查找表
 * ============================================================
 * 中心值 = 半振幅（使谷值从 0V 开始）
 *   例：2Vpp(2000mVpp), VREF=3300mV
 *     半振幅 = 1000mV → center_code = 1000×4096/3300 = 1241
 *     振幅码 = 1000×4096/3300 = 1241
 *     峰值 = 1241+1241 = 2482 (2.0V)
 *     谷值 = 1241-1241 = 0 (0V)
 * 表项预格式化为 2 字节 SPI 数据（PD=00 已嵌入）
 * ============================================================ */
static void build_sine_lut(dac7311_handle_t* handle, uint16_t amplitude_mv)
{
    // 中心 = 半振幅，谷值从 0V 开始
    uint16_t half_amp_mv = amplitude_mv / 2;
    uint16_t center_code = (uint16_t)(((uint32_t)half_amp_mv * 4096UL) / DAC_VREF_MV);
    uint16_t amp_code = center_code;

    for (int i = 0; i < DAC7311_SINE_LUT_SIZE; i++) {
        float angle = 2.0f * (float)M_PI * (float)i / DAC7311_SINE_LUT_SIZE;
        int32_t code = (int32_t)center_code + (int32_t)((float)amp_code * sinf(angle));
        if (code < 0) code = 0;
        if (code > (int32_t)DAC7311_MAX_VALUE) code = DAC7311_MAX_VALUE;

        uint16_t tx = (DAC7311_CMD_NORMAL << 14) | (((uint16_t)code & 0xFFF) << 2);
        handle->sine_lut[i][0] = (uint8_t)((tx >> 8) & 0xFF);
        handle->sine_lut[i][1] = (uint8_t)(tx & 0xFF);
    }
    handle->sine_amplitude_mv = amplitude_mv;
}

/* ============================================================
 * waveform_timer_cb — 定时器回调，正弦/脉冲共用，按模式分发
 * ============================================================ */

/* ---- GPTimer 硬件定时器 ISR：精准触发，只发通知给高优先级任务 ---- */
static bool IRAM_ATTR waveform_timer_isr(gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *edata, void *arg)
{
    dac7311_handle_t *h = (dac7311_handle_t *)arg;
    BaseType_t high_task_awoken = pdFALSE;
    vTaskNotifyGiveFromISR(h->waveform_task, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}

/* ---- 波形生成任务：最高优先级，等 GPTimer 通知 → SPI 发出 ---- */
static void waveform_task(void *arg)
{
    dac7311_handle_t *h = (dac7311_handle_t *)arg;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    // 阻塞等定时器通知

        switch (h->waveform_mode) {

        case DAC7311_MODE_SINE: {
            spi_transaction_t trans = { .length = 16, .tx_buffer = h->sine_lut[h->sine_phase] };
            spi_device_transmit(h->spi_handle, &trans);
            h->sine_phase = (h->sine_phase + 1) & 0x1FF;  // 512 点回绕
            break;
        }

        case DAC7311_MODE_PULSE: {
            uint16_t code = h->pulse_state ? h->pulse_high_code : h->pulse_low_code;
            uint16_t tx = (DAC7311_CMD_NORMAL << 14) | ((code & 0xFFF) << 2);
            spi_transaction_t trans = { .flags = SPI_TRANS_USE_TXDATA, .length = 16 };
            trans.tx_data[0] = (tx >> 8) & 0xFF;
            trans.tx_data[1] = tx & 0xFF;
            spi_device_transmit(h->spi_handle, &trans);
            h->pulse_state = !h->pulse_state;
            break;
        }

        default:
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ---- 启动 GPTimer + 创建任务 ---- */
static esp_err_t waveform_start(dac7311_handle_t* handle, uint64_t period_us)
{
    // 创建 GPTimer
    gptimer_config_t tcfg = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,               // 1 µs 分辨率
    };
    esp_err_t ret = gptimer_new_timer(&tcfg, &handle->waveform_timer);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "GPTimer create failed"); return ret; }

    gptimer_alarm_config_t alarm = {
        .alarm_count = period_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    gptimer_set_alarm_action(handle->waveform_timer, &alarm);

    gptimer_event_callbacks_t cbs = { .on_alarm = waveform_timer_isr };
    gptimer_register_event_callbacks(handle->waveform_timer, &cbs, handle);

    // 创建最高优先级任务（高于 BLE）
    BaseType_t created = xTaskCreate(waveform_task, "dac_wave", 2048, handle,
                                     configMAX_PRIORITIES - 2, &handle->waveform_task);
    if (created != pdPASS) {
        gptimer_del_timer(handle->waveform_timer);
        ESP_LOGE(TAG, "Task create failed");
        return ESP_FAIL;
    }

    gptimer_enable(handle->waveform_timer);
    gptimer_start(handle->waveform_timer);
    return ESP_OK;
}

/* ============================================================
 * dac7311_start_sine
 * ============================================================ */
int dac7311_start_sine(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);

    build_sine_lut(handle, amplitude_mv);
    handle->sine_freq_hz = freq_hz;
    handle->sine_phase = 0;
    handle->waveform_mode = DAC7311_MODE_SINE;

    uint64_t period_us = 1000000ULL / ((uint32_t)freq_hz * DAC7311_SINE_LUT_SIZE);
    esp_err_t ret = waveform_start(handle, period_us);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "SINE: %u Hz, %u mVpp, %u sps, %llu us",
             freq_hz, amplitude_mv, freq_hz * DAC7311_SINE_LUT_SIZE, period_us);
    return ESP_OK;
}

/* ============================================================
 * dac7311_start_pulse
 * ============================================================ */
int dac7311_start_pulse(dac7311_handle_t* handle, uint16_t freq_hz, uint16_t amplitude_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);

    uint16_t high_code = (uint16_t)(((uint32_t)amplitude_mv * 4096UL) / DAC_VREF_MV);
    handle->pulse_high_code = (high_code > DAC7311_MAX_VALUE) ? DAC7311_MAX_VALUE : high_code;
    handle->pulse_low_code = 0;
    handle->pulse_state = true;
    handle->waveform_mode = DAC7311_MODE_PULSE;

    uint64_t half_period_us = 1000000ULL / ((uint32_t)freq_hz * 2);
    esp_err_t ret = waveform_start(handle, half_period_us);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "PULSE: %u Hz, %u mVpp, 50%% duty, high=%u low=%u, %llu us",
             freq_hz, amplitude_mv, handle->pulse_high_code, handle->pulse_low_code, half_period_us);
    return ESP_OK;
}

/* ============================================================
 * dac7311_stop_waveform
 * ============================================================ */
int dac7311_stop_waveform(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->waveform_mode == DAC7311_MODE_NONE) return ESP_OK;

    gptimer_stop(handle->waveform_timer);
    gptimer_disable(handle->waveform_timer);
    gptimer_del_timer(handle->waveform_timer);
    handle->waveform_timer = NULL;

    if (handle->waveform_task) {
        vTaskDelete(handle->waveform_task);
        handle->waveform_task = NULL;
    }
    handle->waveform_mode = DAC7311_MODE_NONE;

    ESP_LOGI(TAG, "Waveform stopped");
    return ESP_OK;
}

/* ============================================================
 * dac7311_power_down / power_up
 * ============================================================ */
int dac7311_power_down(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    dac7311_stop_waveform(handle);
    uint16_t tx = (DAC7311_CMD_POWERDOWN << 14);  // PD=10, data=0
    spi_transaction_t trans = { .flags = SPI_TRANS_USE_TXDATA, .length = 16 };
    trans.tx_data[0] = (tx >> 8) & 0xFF;
    trans.tx_data[1] = tx & 0xFF;
    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    if (ret == ESP_OK) ESP_LOGI(TAG, "DAC powered down");
    return ret;
}

int dac7311_power_up(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return dac7311_write(handle, 0);            // PD=00 + DATA=0
}
