#include "bluetooth.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_log.h"

#if !CONFIG_BT_ENABLED
static const char *TAG = "bluetooth";
esp_err_t bluetooth_init(void) { ESP_LOGE(TAG, "BT disabled"); return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bluetooth_send_key_event(uint8_t p) { (void)p; return ESP_ERR_NOT_SUPPORTED; }
bool bluetooth_is_connected(void) { return false; }
bool bluetooth_is_streaming_enabled(void) { return false; }
esp_err_t bluetooth_send_sensor_data(const sensor_data_t *d) { (void)d; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bluetooth_push_adc_sample(uint16_t r, int16_t v) { (void)r; (void)v; return ESP_ERR_NOT_SUPPORTED; }
void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *ctx) { (void)cb; (void)ctx; }
esp_err_t bluetooth_notify_text(const char *t) { (void)t; return ESP_ERR_NOT_SUPPORTED; }
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

static const ble_uuid16_t g_svc_uuid = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t g_rx_uuid  = BLE_UUID16_INIT(0xFFF1);
static const ble_uuid16_t g_tx_uuid  = BLE_UUID16_INIT(0xFFF2);

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_tx_val_handle = 0;
static bool g_notify_enabled = false;
static bool g_streaming_enabled = false;
static uint8_t g_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static char g_last_tx_value[160] = "STATE RX_OFF";
static TaskHandle_t streaming_task_handle = NULL;
static sensor_data_t g_latest_sensor_data = {0};
static bluetooth_command_cb_t g_command_cb = NULL;
static void *g_command_cb_ctx = NULL;
static adc_ring_buf_t g_adc_ring = {0};

static void ble_app_advertise(void);
static void streaming_task(void *param);

/* ── notify_text ── */
static esp_err_t notify_text(const char *s)
{
    strncpy(g_last_tx_value, s, sizeof(g_last_tx_value) - 1);
    g_last_tx_value[sizeof(g_last_tx_value) - 1] = '\0';
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled) return ESP_OK;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s, strlen(s));
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t bluetooth_notify_text(const char *text) { return notify_text(text); }

/* ── 命令解析 ── */
static void handle_text_command(const uint8_t *data, uint16_t len)
{
    char buf[160];
    size_t n = (len >= sizeof(buf)) ? (sizeof(buf) - 1) : len;
    memcpy(buf, data, n); buf[n] = '\0';
    while (n > 0 && (buf[n-1]=='\n'||buf[n-1]=='\r'||buf[n-1]==' '||buf[n-1]=='\t'))
        { buf[n-1]='\0'; n--; }
    if (strncasecmp(buf, "ADC", 3) == 0) {
        char resp[128];
        snprintf(resp, sizeof(resp), "ADC RAW=%u V=%d mV",
                 g_latest_sensor_data.adc_raw, g_latest_sensor_data.adc_voltage_mv);
        notify_text(resp);
        return;
    }
    if (strncasecmp(buf, "DC", 2) == 0 ||
        strcasecmp(buf, "SINE") == 0 || strcasecmp(buf, "PLU") == 0) {
        if (g_command_cb) g_command_cb(buf, g_command_cb_ctx);
        else notify_text("ERROR: No DAC handler");
        return;
    }
    notify_text("ERROR");
}

/* ── GATT access ── */
static int gatt_svr_chr_access_cb(uint16_t ch, uint16_t ah,
                                  struct ble_gatt_access_ctxt *ctx, void *arg)
{
    (void)ch; (void)ah; (void)arg;
    const ble_uuid_t *uuid = ctx->chr->uuid;
    if (ble_uuid_cmp(uuid, &g_rx_uuid.u) == 0) {
        if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            handle_text_command(ctx->om->om_data, ctx->om->om_len);
            return 0;
        }
        if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            int rc = os_mbuf_append(ctx->om, g_last_tx_value, strlen(g_last_tx_value));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ble_uuid_cmp(uuid, &g_tx_uuid.u) == 0) {
        if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            int rc = os_mbuf_append(ctx->om, g_last_tx_value, strlen(g_last_tx_value));
            return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT service ── */
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &g_svc_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {
        { .uuid = &g_rx_uuid.u, .access_cb = gatt_svr_chr_access_cb,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
        { .uuid = &g_tx_uuid.u, .access_cb = gatt_svr_chr_access_cb,
          .val_handle = &g_tx_val_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
        {0}
    },
}, {0}};

/* ── BLE events ── */
static void ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc) { ESP_LOGE(TAG, "id_infer_auto rc=%d", rc); return; }
    ble_app_advertise();
}
static void ble_on_reset(int reason) { ESP_LOGW(TAG, "Reset reason=%d", reason); }

static int ble_gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            g_conn_handle = ev->connect.conn_handle;
            g_notify_enabled = false;
            g_streaming_enabled = false;
            ESP_LOGI(TAG, "Connected handle=%d", g_conn_handle);
        } else {
            ESP_LOGI(TAG, "Connect fail status=%d", ev->connect.status);
            ble_app_advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected reason=%d", ev->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_notify_enabled = false;
        g_streaming_enabled = false;
        g_adc_ring.head = g_adc_ring.tail = g_adc_ring.count = 0;
        memset(&g_latest_sensor_data, 0, sizeof(g_latest_sensor_data));
        if (streaming_task_handle) { vTaskDelete(streaming_task_handle); streaming_task_handle = NULL; }
        ble_app_advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == g_tx_val_handle) {
            g_notify_enabled = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "Notify %s", g_notify_enabled ? "ENABLED" : "DISABLED");
            if (g_notify_enabled && !streaming_task_handle)
                xTaskCreate(streaming_task, "stream", 2560, NULL, 5, &streaming_task_handle);
            else if (!g_notify_enabled && streaming_task_handle)
                { vTaskDelete(streaming_task_handle); streaming_task_handle = NULL; }
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE: ble_app_advertise(); return 0;
    default: return 0;
    }
}

static void ble_app_advertise(void)
{
    struct ble_gap_adv_params ap = {0};
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.name = (const uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    ble_gap_adv_set_fields(&f);
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(g_own_addr_type, NULL, BLE_HS_FOREVER, &ap, ble_gap_event, NULL);
}

/* ── streaming_task: 文本格式批量推送, 每 25ms 发送 ≤20 采样点 ── */
#define MAX_SPF 20
static void streaming_task(void *param)
{
    (void)param;
    while (1) {
        if (g_notify_enabled && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            // 每次只发一个采样点: "ADC:raw,mv"
            if (g_adc_ring.count > 0) {
                uint16_t r = g_adc_ring.raw[g_adc_ring.tail];
                int16_t  v = g_adc_ring.voltage_mv[g_adc_ring.tail];
                g_adc_ring.tail = (g_adc_ring.tail + 1) % ADC_RING_SIZE;
                g_adc_ring.count--;

                char buf[40];
                int len = snprintf(buf, sizeof(buf), "ADC:%u,%d", r, v);
                struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
                if (om) {
                    int rc = ble_gatts_notify_custom(g_conn_handle, g_tx_val_handle, om);
                    if (rc) ESP_LOGW(TAG, "notify rc=%d", rc);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1)); // 1 tick = 10ms → 100 SPS
    }
}

/* ── BLE init ── */
static void host_task(void *p) { (void)p; nimble_port_run(); nimble_port_freertos_deinit(); }

esp_err_t bluetooth_init(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND)
        { ESP_ERROR_CHECK(nvs_flash_erase()); r = nvs_flash_init(); }
    ESP_ERROR_CHECK(r);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(gatt_svr_svcs)) return ESP_FAIL;
    if (ble_gatts_add_svcs(gatt_svr_svcs)) return ESP_FAIL;
    ble_svc_gap_device_name_set("ESP32_STIM");
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

/* ── public wrappers ── */
esp_err_t bluetooth_send_key_event(uint8_t p) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled || !g_streaming_enabled)
        return ESP_ERR_INVALID_STATE;
    char m[32]; snprintf(m, sizeof(m), "KEY %u", (unsigned)p);
    return notify_text(m);
}
bool bluetooth_is_connected(void)         { return g_conn_handle != BLE_HS_CONN_HANDLE_NONE; }
bool bluetooth_is_streaming_enabled(void) { return g_streaming_enabled; }

esp_err_t bluetooth_send_sensor_data(const sensor_data_t *d) {
    if (!d) return ESP_ERR_INVALID_ARG;
    memcpy(&g_latest_sensor_data, d, sizeof(sensor_data_t));
    return ESP_OK;
}

esp_err_t bluetooth_push_adc_sample(uint16_t raw, int16_t mv) {
    if (g_adc_ring.count >= ADC_RING_SIZE) {
        g_adc_ring.tail = (g_adc_ring.tail + 1) % ADC_RING_SIZE;
        g_adc_ring.count--;
    }
    g_adc_ring.raw[g_adc_ring.head] = raw;
    g_adc_ring.voltage_mv[g_adc_ring.head] = mv;
    g_adc_ring.head = (g_adc_ring.head + 1) % ADC_RING_SIZE;
    g_adc_ring.count++;
    return ESP_OK;
}

void bluetooth_register_command_callback(bluetooth_command_cb_t cb, void *ctx) {
    g_command_cb = cb; g_command_cb_ctx = ctx;
}
#endif
