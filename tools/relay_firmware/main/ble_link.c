#include "ble_link.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_link";

// Must match main/hal/hal_radio.cpp on the badge.
static const uint16_t kCompanyId  = 0xFFFF;
static const uint8_t  kAdvInstance = 0;

// 0.625 ms units. ~200 ms advertising interval, same as the badge.
static const int32_t kAdvItvlMin = 320;
static const int32_t kAdvItvlMax = 352;

// 0.625 ms units. window == itvl, i.e. scan continuously. The badge uses a 20%
// duty cycle to save battery; we have no such constraint and would rather catch
// every repeat of a request.
static const uint16_t kScanItvl   = 160;
static const uint16_t kScanWindow = 160;

static ble_link_rx_cb_t s_rx_cb;
static volatile bool    s_host_synced;
static uint8_t          s_addr[6];

static int on_gap_event(struct ble_gap_event *event, void *arg);

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_host_sync(void)
{
    // Static random address derived from the WiFi MAC, so the relay keeps the
    // same identity across reboots. Top two bits must be 11 per the BT spec.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[5] |= 0xC0;

    int rc = ble_hs_id_set_rnd(mac);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_set_rnd: %d", rc);
        return;
    }
    memcpy(s_addr, mac, 6);
    s_host_synced = true;
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

esp_err_t ble_link_set_payload(const void *data, size_t len)
{
    // 247, not 250: while advertising, the whole AD blob must fit one HCI
    // LE Set Extended Advertising Data command (251) and the wrapper below
    // costs 4. Larger is rejected by the host with EINVAL, silently.
    if (!data || len == 0 || len > BLE_LINK_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;

    // AD structure: [length][type=0xFF][company_lo][company_hi][payload...]
    // The length byte does not count itself.
    uint8_t buf[260];
    size_t total = len + 4;
    if (total > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = (uint8_t)(len + 3);
    buf[1] = 0xFF;
    buf[2] = (uint8_t)(kCompanyId & 0xFF);
    buf[3] = (uint8_t)((kCompanyId >> 8) & 0xFF);
    memcpy(buf + 4, data, len);

    struct os_mbuf *mb = ble_hs_mbuf_from_flat(buf, total);
    if (!mb) return ESP_ERR_NO_MEM;

    int rc = ble_gap_ext_adv_set_data(kAdvInstance, mb);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_set_data: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t configure_adv(void)
{
    struct ble_gap_ext_adv_params params = {0};
    params.connectable        = 0;
    params.scannable          = 0;
    params.directed           = 0;
    params.high_duty_directed = 0;
    params.legacy_pdu         = 0;      // BLE 5.0 extended PDU
    params.anonymous          = 0;
    params.include_tx_power   = 0;
    params.scan_req_notif     = 0;
    params.itvl_min           = kAdvItvlMin;
    params.itvl_max           = kAdvItvlMax;
    params.channel_map        = 7;      // primary channels 37/38/39
    params.own_addr_type      = BLE_OWN_ADDR_RANDOM;
    params.primary_phy        = BLE_HCI_LE_PHY_1M;
    params.secondary_phy      = BLE_HCI_LE_PHY_1M;
    params.tx_power           = 127;    // controller default
    params.sid                = 0;

    int rc = ble_gap_ext_adv_configure(kAdvInstance, &params, NULL,
                                       on_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_configure: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ble_link_init(ble_link_rx_cb_t cb)
{
    s_rx_cb = cb;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    s_host_synced = false;
    nimble_port_freertos_init(host_task);

    int waited_ms = 0;
    while (!s_host_synced && waited_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited_ms += 10;
    }
    if (!s_host_synced) {
        ESP_LOGE(TAG, "host sync timeout");
        return ESP_FAIL;
    }

    err = configure_adv();
    if (err != ESP_OK) return err;

    // Advertising needs data before it will start; main.c replaces this with a
    // real BEACON frame as soon as it's up.
    uint8_t placeholder[4] = {
        0x03, 0xFF,
        (uint8_t)(kCompanyId & 0xFF),
        (uint8_t)((kCompanyId >> 8) & 0xFF),
    };
    struct os_mbuf *mb = ble_hs_mbuf_from_flat(placeholder, sizeof(placeholder));
    if (mb) ble_gap_ext_adv_set_data(kAdvInstance, mb);

    int rc = ble_gap_ext_adv_start(kAdvInstance, 0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_adv_start: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_ext_disc_params scan = {0};
    scan.itvl    = kScanItvl;
    scan.window  = kScanWindow;
    scan.passive = 1;
    rc = ble_gap_ext_disc(BLE_OWN_ADDR_RANDOM,
                          /*duration=*/0,
                          /*period=*/0,
                          /*filter_duplicates=*/0,
                          BLE_HCI_SCAN_FILT_NO_WL,
                          /*limited=*/0,
                          &scan,
                          /*coded=*/NULL,
                          on_gap_event,
                          NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ext_disc: %d", rc);
        ble_gap_ext_adv_stop(kAdvInstance);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE link up, addr %02x:%02x:%02x:%02x:%02x:%02x",
             s_addr[5], s_addr[4], s_addr[3], s_addr[2], s_addr[1], s_addr[0]);
    return ESP_OK;
}

static int on_gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_EXT_DISC) return 0;

    // Advertising data over 229 bytes — the most that fits one HCI Extended
    // Advertising Report — arrives split across events, and NimBLE does not
    // reassemble. A fragment is the front of a longer frame, not a short one,
    // so parsing it would hand the caller half a payload. Everything on this
    // channel fits one report by construction (BLE_LINK_MAX_PAYLOAD); this
    // firing means someone else's advertisement, or a regression.
    if (event->ext_disc.data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
        static uint32_t s_frag_count = 0;
        if ((s_frag_count++ % 64) == 0) {
            ESP_LOGW(TAG, "dropped a fragmented advertisement (%u bytes, %lu so far)",
                     (unsigned)event->ext_disc.length_data,
                     (unsigned long)s_frag_count);
        }
        return 0;
    }

    const uint8_t *data = event->ext_disc.data;
    int dlen = event->ext_disc.length_data;

    int p = 0;
    while (p + 1 < dlen) {
        uint8_t flen = data[p];
        if (flen == 0 || p + 1 + flen > dlen) break;
        uint8_t ftype = data[p + 1];
        if (ftype == 0xFF && flen >= 3) {
            uint16_t cid = (uint16_t)data[p + 2] | ((uint16_t)data[p + 3] << 8);
            if (cid == kCompanyId && flen > 3) {
                if (s_rx_cb) s_rx_cb(data + p + 4, (size_t)flen - 3);
                break;
            }
        }
        p += 1 + flen;
    }
    return 0;
}
