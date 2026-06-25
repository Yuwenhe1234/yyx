/**
 * ============================================================
 * DAC7311 驱动 — 12 位 SPI 电压输出型 DAC
 * ============================================================
 *
 * 【芯片简介】
 *   TI 公司 12 位 DAC，SPI 接口，8 脚封装。输出 = 2×VREF×(CODE/4096)。
 *
 * 【SPI 通信协议】
 *   - 3 线 SPI（无 MISO），只写不读
 *   - 时序：CS↓ → 发 16 个 bit → CS↑ → 输出电压立即更新
 *   - 模式：SPI Mode 1（CPOL=0 空闲低, CPHA=1 下降沿采样）
 *     数据手册明确："data loaded on the falling edge of SCLK"
 *
 * 【16 位数据帧】
 *   DB[15:14] = PD1,PD0（掉电控制：00=正常, 01=1kΩ, 10=100kΩ, 11=高阻）
 *   DB[13:12] = 无关位
 *   DB[11:0]  = 12 位 DAC 编码（0→0V, 4095→满量程）
 *
 * 【代码结构】
 *   dac7311_init()        初始化：内存 → SPI2 总线 → 挂设备 → 输出 0V
 *   dac7311_deinit()      释放：拔设备 → 释放总线 → 释放内存
 *   dac7311_write()       核心：12bit→拼 16bit 帧→SPI 发出
 *   dac7311_write_voltage() 封装：毫伏→换算 12bit→调 write
 *   dac7311_power_down()  掉电：PD=10，输出接 100kΩ 到地
 *   dac7311_power_up()    唤醒：PD=00 + DATA=0，正常输出 0V
 * ============================================================ */

#include "dac7311.h"
#include "esp_log.h"
#include "esp_err.h"
#include "stdlib.h"

static const char *TAG = "DAC7311";                     // 日志标签

/* ============================================================
 * dac7311_init — 初始化
 * ============================================================
 * 三步：配 SPI2 总线 → 挂 DAC7311 设备 → 写初始值输出 0V
 * 成功返回句柄指针，失败返回 NULL
 * ============================================================ */
dac7311_handle_t* dac7311_init(void)
{
    // 分配句柄内存
    dac7311_handle_t* handle = (dac7311_handle_t*)malloc(sizeof(dac7311_handle_t));
    if (!handle) {
        ESP_LOGE(TAG, "Failed to allocate memory for DAC7311 handle");
        return NULL;
    }

    // ---- 第 1 步：初始化 SPI2 总线硬件 ----
    spi_bus_config_t bus_config = {
        .mosi_io_num = DAC7311_MOSI_GPIO,   // MOSI→GPIO23→DAC7311 第6脚 DIN
        .miso_io_num = DAC7311_MISO_GPIO,   // MISO→GPIO19（DAC7311 无此脚，保留）
        .sclk_io_num = DAC7311_SCLK_GPIO,   // SCLK→GPIO18→DAC7311 第5脚 SCLK
        .quadwp_io_num = -1,                // Quad SPI 不用
        .quadhd_io_num = -1,                // Quad SPI 不用
        .max_transfer_sz = 4,               // 最大 4 字节（实际只发 2 字节，够了）
    };

    // SPI_DMA_CH_AUTO：驱动自动分配 DMA 通道
    esp_err_t ret = spi_bus_initialize(DAC7311_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        free(handle);
        return NULL;
    }

    // ---- 第 2 步：挂载 DAC7311 到 SPI 总线 ----
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = DAC7311_SPI_FREQ, // 1 MHz（DAC7311 最高 50 MHz）
        .mode = 1,                          // SPI Mode 1：CPOL=0 空闲低, CPHA=1 下降沿采样
                                            // DAC7311 手册写："data loaded on falling edge"
        .spics_io_num = DAC7311_CS_GPIO,    // CS→GPIO5→DAC7311 第4脚 SYNC
                                            // 驱动自动：发前拉低 CS，16 个 bit 发完拉高 CS
        .queue_size = 1,                    // 命令队列深度（只用同步传输，1 够了）
        .flags = SPI_DEVICE_NO_DUMMY,       // DAC7311 不需要 dummy bit
    };

    ret = spi_bus_add_device(DAC7311_SPI_HOST, &dev_config, &handle->spi_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(ret));
        spi_bus_free(DAC7311_SPI_HOST);
        free(handle);
        return NULL;
    }

    ESP_LOGI(TAG, "DAC7311 initialized successfully");

    // ---- 第 3 步：上电输出 0V ----
    // DAC7311 上电默认就是 0V，这里显式写一次确保状态正确
    dac7311_write(handle, 0);

    return handle;
}

/* ============================================================
 * dac7311_deinit — 释放所有资源
 * ============================================================
 * 顺序不能乱：先拔设备 → 再释放总线 → 最后释放句柄
 * ============================================================ */
void dac7311_deinit(dac7311_handle_t* handle)
{
    if (!handle) return;                    // 空指针保护

    spi_bus_remove_device(handle->spi_handle);  // 从 SPI 总线移除设备
    spi_bus_free(DAC7311_SPI_HOST);             // 释放 SPI2 总线硬件
    free(handle);                               // 释放句柄内存

    ESP_LOGI(TAG, "DAC7311 deinitialized");
}

/* ============================================================
 * dac7311_write — 核心写入，把 12 位值通过 SPI 发给 DAC7311
 * ============================================================
 *
 * 【数据拼装流程】（以 value=2048=0x800 为例）
 *
 *   value = 2048 = 0b 0000_1000_0000_0000
 *
 *   ① 取低 12 位：  value & 0xFFF          = 0x0800
 *   ② 加掉电位：    (0x00 << 14) | 0x0800  = 0x0800
 *      DB15=0, DB14=0 → PD=00 → 正常模式
 *      DB13=0, DB12=0 → 无关位
 *      DB11~DB0 = 0x800 → DAC 数据
 *
 *   ③ 拆为发送字节（高位先发）：
 *      data[0] = 0x08  （DB15~DB8）
 *      data[1] = 0x00  （DB7~DB0）
 *
 *   ④ SPI 硬件自动：
 *      拉低 CS → 发 16 个时钟 + 数据 → 拉高 CS → DAC 输出更新
 *
 * ============================================================ */
int dac7311_write(dac7311_handle_t* handle, uint16_t value)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // 值域钳位
    if (value > DAC7311_MAX_VALUE) {
        ESP_LOGW(TAG, "DAC value out of range, clamping to max");
        value = DAC7311_MAX_VALUE;
    }

    // 拼装 16 位 SPI 帧
    // = [PD1,PD0 | X,X | D11,D10,...,D0]
    // DAC7311_CMD_NORMAL=0x00 → 左移14位 → PD=00（正常工作）
    // value & 0xFFF → 12位数据放在低12位
    uint16_t tx_data = (DAC7311_CMD_NORMAL << 14) | (value & 0xFFF);

    // 拆成两个字节，大端序（高位在前）
    uint8_t data[2];
    data[0] = (tx_data >> 8) & 0xFF;        // DB[15:8]：PD+无关+高4位数据
    data[1] = tx_data & 0xFF;               // DB[7:0]：低8位数据

    // SPI 事务：16 位，只发不收
    spi_transaction_t trans = {
        .length = 16,                       // 正好一个 DAC7311 帧
        .tx_buffer = data,                  // 发送缓冲区
    };

    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write DAC value: %s", esp_err_to_name(ret));
    }

    return ret;
}

/* ============================================================
 * dac7311_write_voltage — 按毫伏值写 DAC
 * ============================================================
 *
 * 换算公式：CODE = voltage_mv × 4095 / vref_mv
 *
 * 例：VREF=2500mV, 要输出 1250mV
 *     CODE = 1250 × 4095 / 2500 = 2047
 *
 * 注意：DAC7311 满量程 = 2×VREF，调用者传 vref_mv 时应考虑此关系。
 * 用 uint32_t 做中间计算，防止 voltage_mv × 4095 溢出 uint16_t。
 * ============================================================ */
int dac7311_write_voltage(dac7311_handle_t* handle, uint16_t voltage_mv, uint16_t vref_mv)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // 电压 → 12 位 DAC 编码，u32 防溢出
    uint32_t dac_value = ((uint32_t)voltage_mv * DAC7311_MAX_VALUE) / vref_mv;

    if (dac_value > DAC7311_MAX_VALUE) {
        dac_value = DAC7311_MAX_VALUE;
        ESP_LOGW(TAG, "Voltage exceeds reference, clamping to max");
    }

    return dac7311_write(handle, (uint16_t)dac_value);
}

/* ============================================================
 * dac7311_power_down — 进入 100kΩ 掉电模式
 * ============================================================
 *
 * 发送帧 = 0x02 << 14 = 0x8000
 *   DB15=1, DB14=0 → PD=10 → 输出经 100kΩ 内阻接地
 *   DAC 内部电路停止工作，降低功耗。
 *   唤醒需调 dac7311_power_up()。
 * ============================================================ */
int dac7311_power_down(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;

    // PD=10 + DATA=0
    uint16_t tx_data = (DAC7311_CMD_POWERDOWN << 14);

    uint8_t data[2];
    data[0] = (tx_data >> 8) & 0xFF;
    data[1] = tx_data & 0xFF;

    spi_transaction_t trans = {
        .length = 16,
        .tx_buffer = data,
    };

    esp_err_t ret = spi_device_transmit(handle->spi_handle, &trans);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to power down DAC: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "DAC7311 powered down");
    }

    return ret;
}

/* ============================================================
 * dac7311_power_up — 退出掉电，恢复正常输出
 * ============================================================
 * 就是 dac7311_write(handle, 0)，语义化封装：
 * PD=00（正常）+ DATA=0 → 退出掉电，输出 0V
 * ============================================================ */
int dac7311_power_up(dac7311_handle_t* handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    return dac7311_write(handle, 0);
}
