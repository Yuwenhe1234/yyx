#include "bluetooth.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"

// 若未开启 BT，则提供桩函数，避免工程无法编译；真正使用需在 menuconfig 启用 BT/NimBLE
#if !CONFIG_BT_ENABLED

static const char *TAG = "bluetooth";

// 蓝牙初始化函数 - 桩函数版本（BT未启用时使用）
// 返回不支持错误，提示用户启用BT配置
esp_err_t bluetooth_init(void)
{
    ESP_LOGE(TAG, "Bluetooth disabled. Enable CONFIG_BT_ENABLED + NimBLE in menuconfig.");
    return ESP_ERR_NOT_SUPPORTED;
}

// 发送按键事件函数 - 桩函数版本
// 忽略输入，返回不支持错误
esp_err_t bluetooth_send_key_event(uint8_t pressed)
{
    (void)pressed;
    return ESP_ERR_NOT_SUPPORTED;
}

// 检查蓝牙连接状态 - 桩函数版本
// 始终返回false
bool bluetooth_is_connected(void) { return false; }

// 检查流媒体启用状态 - 桩函数版本
// 始终返回false
bool bluetooth_is_streaming_enabled(void) { return false; }

// 推送传感器数据 - 桩函数版本
esp_err_t bluetooth_send_sensor_data(const sensor_data_t *data)
{
    (void)data;
    return ESP_ERR_NOT_SUPPORTED;
}

// 注册命令回调 - 桩函数版本
void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *user_ctx)
{
    (void)cb;
    (void)user_ctx;
}

#else

#include "nvs_flash.h"

#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bluetooth";

// 16-bit UUID demo: FFF0 service, FFF1 RX(write), FFF2 TX(notify/read)
static const ble_uuid16_t g_svc_uuid = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t g_rx_uuid  = BLE_UUID16_INIT(0xFFF1);
static const ble_uuid16_t g_tx_uuid  = BLE_UUID16_INIT(0xFFF2);

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_tx_val_handle = 0;
static bool g_notify_enabled = false;
static bool g_streaming_enabled = false; // 手机端“开始接收”命令开关
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static char g_last_tx_value[160] = "STATE RX_OFF";
static TaskHandle_t streaming_task_handle = NULL;
static sensor_data_t g_latest_sensor_data = {0};
static bluetooth_command_cb_t g_command_cb = NULL;
static void *g_command_cb_ctx = NULL;

static void ble_app_advertise(void);
static void streaming_task(void *param);

// 通知文本消息函数
// 通过BLE通知发送文本字符串给连接的客户端
static esp_err_t notify_text(const char *s)
{
    // 缓存最新消息，支持客户端直接读取TX特征（不依赖通知订阅）
    strncpy(g_last_tx_value, s, sizeof(g_last_tx_value) - 1);
    g_last_tx_value[sizeof(g_last_tx_value) - 1] = '\0';

    // 只有在连接且通知已启用时，才发送 notify
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled) {
        return ESP_OK;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
    if (rc != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 设置流媒体状态函数
// 启用或禁用流媒体，并发送确认消息
static void set_streaming(bool en)
{
    g_streaming_enabled = en;
    (void)notify_text(en ? "ACK RX_ON" : "ACK RX_OFF");
}

// 处理文本命令函数
// 解析从客户端接收到的文本命令并执行相应操作
static void handle_text_command(const uint8_t *data, uint16_t len)
{
    char buf[160];
    // 复制数据到缓冲区，确保不溢出
    size_t n = (len >= sizeof(buf)) ? (sizeof(buf) - 1) : len;
    memcpy(buf, data, n);
    buf[n] = '\0';

    // 去除尾部空白字符
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        n--;
    }

    // 处理 ADC 查询指令：返回缓存的 ADS1013 外部 ADC 数据
    if (strncasecmp(buf, "ADC", 3) == 0) {
        char resp[128];
        snprintf(resp, sizeof(resp), "ADC RAW=%u V=%d mV",
                 g_latest_sensor_data.adc_raw,
                 g_latest_sensor_data.adc_voltage_mv);
        (void)notify_text(resp);
        return;
    }

    // 处理 DC/SINE/PLU 指令：转发到 main.c 注册的回调函数（操作外部 DAC7311）
    if (strncasecmp(buf, "DC", 2) == 0 ||
        strcasecmp(buf, "SINE") == 0 ||
        strcasecmp(buf, "PLU") == 0) {
        if (g_command_cb) {
            g_command_cb(buf, g_command_cb_ctx);
        } else {
            (void)notify_text("ERROR: No DAC handler");
        }
        return;
    }

    (void)notify_text("ERROR");
}

// GATT服务器特征访问回调函数
// 处理客户端对GATT特征的读写操作
static int gatt_svr_chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    const ble_uuid_t *uuid = ctxt->chr->uuid;

    // 处理RX特征（读写操作）
    if (ble_uuid_cmp(uuid, &g_rx_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            const uint8_t *data = ctxt->om->om_data;
            uint16_t len = ctxt->om->om_len;
            handle_text_command(data, len);
            return 0;
        }
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *s = g_last_tx_value;
            int rc = os_mbuf_append(ctxt->om, s, strlen(s));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    // 处理TX特征（读操作）
    if (ble_uuid_cmp(uuid, &g_tx_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            const char *s = g_last_tx_value;
            int rc = os_mbuf_append(ctxt->om, s, strlen(s));
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// GATT服务定义
// 定义BLE GATT服务结构，包括主服务和两个特征
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &g_rx_uuid.u,
                .access_cb = gatt_svr_chr_access_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &g_tx_uuid.u,
                .access_cb = gatt_svr_chr_access_cb,
                .val_handle = &g_tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {0}
        },
    },
    {0},
};

// BLE同步回调函数
// 当BLE主机栈同步时调用，推断地址类型并开始广播
static void ble_on_sync(void)
{
    // NimBLE 会在这里写入 own_addr_type，不能传 NULL，否则会崩溃
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed; rc=%d", rc);
        return;
    }
    ble_app_advertise();
}

// BLE重置回调函数
// 当BLE主机栈重置时调用，记录原因
static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "Resetting state; reason=%d", reason);
}

// BLE GAP事件处理函数
// 处理BLE连接、断开、订阅等事件
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        // 连接事件
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            g_notify_enabled = false;
            g_streaming_enabled = false;
            ESP_LOGI(TAG, "Connected. handle=%d", g_conn_handle);
        } else {
            ESP_LOGI(TAG, "Connect failed; status=%d", event->connect.status);
            ble_app_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        // 断开连接事件
        ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_notify_enabled = false;
        g_streaming_enabled = false;
        if (streaming_task_handle != NULL) {
            vTaskDelete(streaming_task_handle);
            streaming_task_handle = NULL;
        }
        ble_app_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // 订阅事件（通知启用/禁用）
        if (event->subscribe.attr_handle == g_tx_val_handle) {
            g_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "Notify %s", g_notify_enabled ? "enabled" : "disabled");
            if (g_notify_enabled) {
                if (streaming_task_handle == NULL) {
                    xTaskCreate(streaming_task, "stream_task", 2048, NULL, 5, &streaming_task_handle);
                }
            } else {
                if (streaming_task_handle != NULL) {
                    vTaskDelete(streaming_task_handle);
                    streaming_task_handle = NULL;
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // 广播完成事件，继续广播
        ble_app_advertise();
        return 0;

    default:
        return 0;
    }
}

// BLE广播函数
// 配置并启动BLE广播，使设备可被发现和连接
static void ble_app_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;

    // 设置广播字段
    memset(&fields, 0, sizeof(fields));
    const char *name = ble_svc_gap_device_name();
    fields.name = (const uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    ble_gap_adv_set_fields(&fields);

    // 设置广播参数
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    // 启动广播
    ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event, NULL);
}

// 传感器数据流式发送任务
// 当notify启用时，持续发送 ADS1013 和 DAC7311 的缓存数据
static void streaming_task(void *param)
{
    (void)param;
    while (1) {
        if (g_notify_enabled && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            if (g_latest_sensor_data.adc_valid || g_latest_sensor_data.dac_valid) {
                char buf[128];
                int len = snprintf(buf, sizeof(buf),
                    "SENSOR ADC_RAW=%u ADC_MV=%d DAC=%u",
                    g_latest_sensor_data.adc_raw,
                    g_latest_sensor_data.adc_voltage_mv,
                    g_latest_sensor_data.dac_value);
                if (len > 0 && len < (int)sizeof(buf)) {
                    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
                    if (om) {
                        ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 每100ms检查并发送
    }
}

// BLE主机任务函数
// 运行NimBLE主机栈的主循环
static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// 蓝牙初始化函数
// 初始化NVS、NimBLE主机栈、配置GATT服务并启动BLE
esp_err_t bluetooth_init(void)
{
    // 初始化NVS闪存
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ESP-IDF v5.2.x: use nimble_port_init() to init controller + host.
    ESP_ERROR_CHECK(nimble_port_init());

    // 配置BLE主机栈回调
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    // 初始化GAP和GATT服务
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // 添加自定义GATT服务
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return ESP_FAIL;
    }

    // 设置设备名称
    ble_svc_gap_device_name_set("ESP32_STIM");

    // 初始化FreeRTOS任务
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

// 发送按键事件函数
// 通过BLE通知发送按键按下事件给连接的客户端
esp_err_t bluetooth_send_key_event(uint8_t pressed)
{
    // 检查连接和通知状态
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled || !g_streaming_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    // 格式化消息
    char msg[32];
    snprintf(msg, sizeof(msg), "KEY %u", (unsigned)pressed);
    return notify_text(msg);
}

// 检查蓝牙连接状态函数
// 返回当前是否与客户端连接
bool bluetooth_is_connected(void)
{
    return g_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

// 检查流媒体启用状态函数
// 返回流媒体是否启用
bool bluetooth_is_streaming_enabled(void)
{
    return g_streaming_enabled;
}

// 推送传感器数据到蓝牙模块
// main.c 调用此函数，将 ADS1013 / DAC7311 的最新读数写入缓存供 BLE 推送
esp_err_t bluetooth_send_sensor_data(const sensor_data_t *data)
{
    if (!data) return ESP_ERR_INVALID_ARG;
    memcpy(&g_latest_sensor_data, data, sizeof(sensor_data_t));
    return ESP_OK;
}

// 注册手机命令回调
// main.c 传入处理函数，蓝牙收到 DC/SINE/PLU 等命令时调用回调
void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *user_ctx)
{
    g_command_cb = cb;
    g_command_cb_ctx = user_ctx;
}

#endif

