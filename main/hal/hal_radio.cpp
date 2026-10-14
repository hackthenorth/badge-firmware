#include "hal_radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "host/ble_hs_mbuf.h"

#include <cstring>
#include <cstdio>
#include <atomic>
#include <mutex>

static const char *TAG = "hal_radio";

// "Test" / unassigned company ID per Bluetooth SIG. Every protocol on this
// channel (BMP2 bump cards, BBX1 relay frames) is discriminated by its own
// 4-byte magic at the start of the manufacturer-specific data block.
static constexpr uint16_t kCompanyId = 0xFFFF;
static constexpr uint8_t  kAdvInstance = 0;

// 0.625 ms units. ~200 ms primary advertising interval.
static constexpr int32_t kAdvItvlMin = 320;
static constexpr int32_t kAdvItvlMax = 352;

// 0.625 ms units. 30 ms window inside a 150 ms interval ⇒ 20% scan duty.
// Detection latency stays well under the 500 ms mutual-bump window.
static constexpr uint16_t kScanItvl   = 240;
static constexpr uint16_t kScanWindow = 48;

// BLE-legal bounds, in 0.625 ms units.
static constexpr uint32_t kAdvUnitsMin  = 0x20;      // 20 ms
static constexpr uint32_t kAdvUnitsMax  = 0x4000;    // 10.24 s
static constexpr uint16_t kScanUnitsMin = 0x0004;    // 2.5 ms
static constexpr uint16_t kScanUnitsMax = 0x4000;    // 10.24 s

// ms -> 0.625 ms units, rounded to nearest.
static constexpr uint32_t ms_to_units(uint32_t ms) { return (ms * 8 + 2) / 5; }

template <typename T>
static T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Live timing values. Retunable at runtime; reset by hal_radio_reset_timings()
// and by every hal_radio_enable() so a retuning app can't leak its settings
// into the next one.
static int32_t  s_adv_itvl_min = kAdvItvlMin;
static int32_t  s_adv_itvl_max = kAdvItvlMax;
static uint16_t s_scan_itvl    = kScanItvl;
static uint16_t s_scan_window  = kScanWindow;

static RadioRecvCb s_recv_cb;
static std::mutex s_cb_mutex;

static bool s_initialized = false;
static bool s_running = false;
static bool s_advertising = false;
static bool s_scanning = false;
static volatile bool s_host_synced = false;
static uint8_t s_addr[6] = {};
static SemaphoreHandle_t s_host_done = nullptr;
// Retain startup failures after the USB log has scrolled away. Reading these
// diagnostics never initializes the controller or transmits anything.
static std::atomic<const char *> s_last_error_stage{"none"};
static std::atomic<int> s_last_error_code{0};

static void record_failure(const char *stage, int code)
{
    s_last_error_code.store(code);
    s_last_error_stage.store(stage);
}

// Serializes enable/disable across the LVGL task (app enter/exit via the
// registry) and the console task (snapshot path).
//
// History: hal_radio_disable() used to run nimble_port_stop()/
// nimble_port_deinit() synchronously on the caller's task. On the app-switch
// path that task is the LVGL task holding lvgl_port_lock, and blocking it
// inside the NimBLE/host + controller teardown wedged the badge — exiting any
// BT app rebooted it. A later attempt deferred the teardown to a worker task,
// which fixed the crash but broke the radio instead: this build uses NimBLE
// static allocation (no BLE_STATIC_TO_DYNAMIC), where nimble_port_deinit() is
// a one-way door — the next nimble_port_init() fails in esp_nimble_hci_init()
// ("hci inits failed") and every radio app reports NO RADIO until reboot.
// Only a fast re-enable that cancelled the still-pending teardown kept
// working, which is why quick launch/exit cycles looked fine.
//
// So the stack is now initialized exactly once per boot and never torn down.
// disable() only silences TX (adv stopped and instance removed) and RX (scan
// cancelled); with modem sleep enabled the idle controller sleeps, and there
// is no re-init path left to fail.
static SemaphoreHandle_t s_radio_mutex = nullptr;
static portMUX_TYPE s_boot_lock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_radio_prims()
{
    portENTER_CRITICAL(&s_boot_lock);
    if (!s_radio_mutex) s_radio_mutex = xSemaphoreCreateMutex();
    portEXIT_CRITICAL(&s_boot_lock);
}

// Last payload handed to hal_radio_send(). Reconfiguring the adv instance
// (which is what retuning the interval requires) drops the instance's data,
// so we re-apply this afterwards — otherwise a mid-broadcast retune would
// silently revert the badge to the placeholder AD.
static uint8_t s_payload[kHalRadioMaxPayload];
static size_t  s_payload_len = 0;

static int on_gap_event(struct ble_gap_event *event, void *arg);

static void host_task(void *)
{
    nimble_port_run();              // never returns: the stack is up for the
                                    // life of the boot (see note at the top)
    if (s_host_done) xSemaphoreGive(s_host_done);
    nimble_port_freertos_deinit();  // self-delete the host task (defensive)
}

// Build a static random address from the chip's WiFi MAC so it's stable across
// reboots (apps use these 6 bytes as peer identity). Per BT spec, the top two
// bits of byte 5 (MSB) must be 11 for "static random".
//
// Pure and cheap, so hal_radio_local_mac() can answer with the radio powered
// down rather than handing back zeros.
static void derive_static_addr(uint8_t out[6])
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[5] |= 0xC0;
    memcpy(out, mac, 6);
}

static void on_host_sync()
{
    uint8_t mac[6];
    derive_static_addr(mac);

    int rc = ble_hs_id_set_rnd(mac);
    if (rc != 0) {
        record_failure("random address (NimBLE)", rc);
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

static esp_err_t init_once()
{
    ESP_LOGI(TAG, "init_once: free heap %u largest %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        record_failure("NVS (ESP-IDF)", err);
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        record_failure("stack init (ESP-IDF)", err);
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    if (!s_host_done) {
        s_host_done = xSemaphoreCreateBinary();
        if (!s_host_done) {
            record_failure("host semaphore (ESP-IDF)", ESP_ERR_NO_MEM);
            ESP_LOGE(TAG, "host_done sem create failed");
            return ESP_ERR_NO_MEM;
        }
    } else {
        // Drain any leftover give from a prior cycle.
        xSemaphoreTake(s_host_done, 0);
    }

    s_host_synced = false;
    nimble_port_freertos_init(host_task);

    // Wait for the host stack to come up and report ready. This runs once
    // per boot, on first entry to a radio app. Budget generously: on a cold
    // boot (RF calibration, NVS init) the sync lands well past one second,
    // and a short budget fails the init and presents NO RADIO even though
    // the stack would have come up moments later.
    ESP_LOGI(TAG, "waiting for host sync (free heap %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    int waited_ms = 0;
    while (!s_host_synced && waited_ms < 5000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (!s_host_synced) {
        if (s_last_error_code.load() == 0)
            record_failure("host sync (ESP-IDF)", ESP_ERR_TIMEOUT);
        ESP_LOGE(TAG, "host sync timeout");
        return ESP_FAIL;
    }

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t configure_adv_instance()
{
    struct ble_gap_ext_adv_params params = {};
    params.connectable = 0;
    params.scannable = 0;
    params.directed = 0;
    params.high_duty_directed = 0;
    params.legacy_pdu = 0;          // BLE 5.0 extended PDU
    params.anonymous = 0;
    params.include_tx_power = 0;
    params.scan_req_notif = 0;
    params.itvl_min = s_adv_itvl_min;
    params.itvl_max = s_adv_itvl_max;
    params.channel_map = 7;         // primary channels 37/38/39
    params.own_addr_type = BLE_OWN_ADDR_RANDOM;
    params.primary_phy = BLE_HCI_LE_PHY_1M;
    params.secondary_phy = BLE_HCI_LE_PHY_1M;
    params.tx_power = 127;          // 127 = controller default
    params.sid = 0;

    int rc = ble_gap_ext_adv_configure(kAdvInstance, &params, nullptr,
                                       on_gap_event, nullptr);
    if (rc != 0) {
        record_failure("advertising setup (NimBLE)", rc);
        ESP_LOGE(TAG, "ble_gap_ext_adv_configure: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_scan()
{
    struct ble_gap_ext_disc_params scan = {};
    scan.itvl = s_scan_itvl;
    scan.window = s_scan_window;
    scan.passive = 1;
    int rc = ble_gap_ext_disc(BLE_OWN_ADDR_RANDOM,
                              /*duration=*/0,
                              /*period=*/0,
                              /*filter_duplicates=*/0,
                              BLE_HCI_SCAN_FILT_NO_WL,
                              /*limited=*/0,
                              &scan,
                              /*coded=*/nullptr,
                              on_gap_event,
                              nullptr);
    if (rc != 0) {
        record_failure("scan start (NimBLE)", rc);
        ESP_LOGE(TAG, "ble_gap_ext_disc: %d", rc);
        return ESP_FAIL;
    }
    s_scanning = true;
    return ESP_OK;
}

static esp_err_t set_adv_payload(const uint8_t *payload, size_t plen)
{
    // AD structure: [length][type=0xFF][company_id_lo][company_id_hi][payload...]
    // length byte does not count itself.
    if (plen + 3 > 0xFE) return ESP_ERR_INVALID_SIZE;

    // Reject here rather than letting NimBLE fail with a bare EINVAL. While
    // advertising, the whole blob must fit one HCI command (251 bytes) and the
    // wrapper below costs 4 of them. Getting this wrong drops frames silently
    // and presents as bad RF, so say what actually happened.
    if (plen > kHalRadioMaxPayload) {
        ESP_LOGE(TAG, "payload %u > %u: the AD blob would not fit one HCI "
                      "advertising report and the peer would drop it",
                 (unsigned)plen, (unsigned)kHalRadioMaxPayload);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[260];
    size_t total = plen + 4;
    if (total > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = (uint8_t)(plen + 3);
    buf[1] = 0xFF;
    buf[2] = (uint8_t)(kCompanyId & 0xFF);
    buf[3] = (uint8_t)((kCompanyId >> 8) & 0xFF);
    memcpy(buf + 4, payload, plen);

    struct os_mbuf *mb = ble_hs_mbuf_from_flat(buf, total);
    if (!mb) {
        record_failure("advertising payload buffer (ESP-IDF)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    int rc = ble_gap_ext_adv_set_data(kAdvInstance, mb);
    if (rc != 0) {
        record_failure("advertising payload (NimBLE)", rc);
        ESP_LOGE(TAG, "ble_gap_ext_adv_set_data: %d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t hal_radio_probe()
{
    ensure_radio_prims();
    if (!s_radio_mutex) {
        record_failure("radio mutex (ESP-IDF)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    esp_err_t result = ESP_OK;
    if (!s_initialized) {
        record_failure("none", 0);
        result = init_once();
    }
    xSemaphoreGive(s_radio_mutex);
    return result;
}

esp_err_t hal_radio_enable()
{
    ensure_radio_prims();
    if (!s_radio_mutex) {
        record_failure("radio mutex (ESP-IDF)", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    record_failure("none", 0);

    esp_err_t result = ESP_OK;
    if (!s_initialized) {
        // Runs once per boot; the stack is never torn down (see note at the
        // top), so there is no re-init path to fail.
        if (init_once() != ESP_OK) result = ESP_FAIL;
    }
    if (result == ESP_OK && s_running) {
        xSemaphoreGive(s_radio_mutex);
        return ESP_OK;
    }

    if (result == ESP_OK) {
        // Timings are deliberately NOT reset here: an app may retune before
        // enabling so its very first advertisement already uses the right
        // interval. Isolation between apps comes from hal_radio_disable(),
        // which the app registry force-calls on every switch.
        if (configure_adv_instance() != ESP_OK) {
            result = ESP_FAIL;
        }
    }

    if (result == ESP_OK) {
        // Placeholder so adv-start has data; apps overwrite via hal_radio_send.
        const uint8_t placeholder = 0;
        result = set_adv_payload(&placeholder, 0);
    }
    if (result == ESP_OK) {
        int rc = ble_gap_ext_adv_start(kAdvInstance, 0, 0);
        if (rc != 0) {
            record_failure("advertising start (NimBLE)", rc);
            ESP_LOGE(TAG, "ble_gap_ext_adv_start: %d", rc);
            result = ESP_FAIL;
        } else {
            s_advertising = true;
        }
    }

    if (result == ESP_OK) {
        if (start_scan() != ESP_OK) {
            ble_gap_ext_adv_stop(kAdvInstance);
            s_advertising = false;
            result = ESP_FAIL;
        }
    }

    if (result == ESP_OK) {
        s_running = true;
        ESP_LOGI(TAG, "BLE radio ready");
    }
    xSemaphoreGive(s_radio_mutex);
    return result;
}

void hal_radio_debug(char *out, size_t capacity)
{
    if (!out || capacity == 0) return;
    const char *stage = s_last_error_stage.load();
    const int code = s_last_error_code.load();
    // ext_adv_active is NimBLE's host bookkeeping, not an on-air TX check.
    snprintf(out, capacity,
             "last_error_stage=%s\nlast_error_code=%d (0x%x)\n"
             "controller_state=%d\nfree_heap=%u\nlargest_heap_block=%u\n"
             "dma_free=%u dma_largest=%u\n"
             "running=%d advertising=%d adv_active=%d scanning=%d\n",
             stage, code, (unsigned)code, (int)esp_bt_controller_get_status(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (int)s_running, (int)s_advertising,
             ble_gap_ext_adv_active(kAdvInstance), (int)s_scanning);
}

void hal_radio_disable()
{
    ensure_radio_prims();
    // If the mutex itself failed to allocate, still silence the radio below
    // (unsynchronised) and skip the lock; a later call retries the bootstrap.
    const bool locked = s_radio_mutex &&
                        xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE;

    // Silence everything NOW. No TX (adv stopped and instance removed) and no
    // RX (scan cancelled) means no RF activity past this point. The NimBLE
    // stack itself stays up (see note at the top): tearing it down is a
    // one-way door on this build, and these calls are single non-blocking HCI
    // commands, so this is safe under lvgl_port_lock.
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (s_advertising) {
        ble_gap_ext_adv_stop(kAdvInstance);
        ble_gap_ext_adv_remove(kAdvInstance);
        s_advertising = false;
    }
    s_running = false;

    // No callbacks past this point. Apps re-register on their next enable.
    {
        std::lock_guard<std::mutex> lk(s_cb_mutex);
        s_recv_cb = RadioRecvCb{};
    }

    // Timings and the cached payload are per-session; never carry them into
    // the next app.
    hal_radio_reset_timings();
    s_payload_len = 0;

    if (locked) xSemaphoreGive(s_radio_mutex);
}

bool hal_radio_is_enabled()
{
    ensure_radio_prims();
    if (!s_radio_mutex) return s_running;
    bool running = false;
    if (xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE) {
        running = s_running;
        xSemaphoreGive(s_radio_mutex);
    }
    return running;
}

esp_err_t hal_radio_send(const void *data, size_t len)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (!data || len == 0 || len > sizeof(s_payload)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = set_adv_payload((const uint8_t *)data, len);
    if (err != ESP_OK) return err;

    memcpy(s_payload, data, len);
    s_payload_len = len;

    // Setting data on a paused instance is legal and stages the payload for
    // the next resume, so callers can prepare a burst then start it.
    return ESP_OK;
}

esp_err_t hal_radio_adv_pause()
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (!s_advertising) return ESP_OK;

    int rc = ble_gap_ext_adv_stop(kAdvInstance);
    // BLE_HS_EALREADY just means the controller already stopped it.
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ext_adv_stop: %d", rc);
        return ESP_FAIL;
    }
    s_advertising = false;
    return ESP_OK;
}

esp_err_t hal_radio_adv_resume()
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    if (s_advertising) return ESP_OK;

    int rc = ble_gap_ext_adv_start(kAdvInstance, 0, 0);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        record_failure("advertising resume (NimBLE)", rc);
        ESP_LOGE(TAG, "ext_adv_start: %d", rc);
        return ESP_FAIL;
    }
    s_advertising = true;
    return ESP_OK;
}

bool hal_radio_is_advertising()
{
    return s_running && s_advertising;
}

esp_err_t hal_radio_set_adv_interval_ms(uint32_t min_ms, uint32_t max_ms)
{
    if (min_ms == 0 || max_ms < min_ms) return ESP_ERR_INVALID_ARG;

    uint32_t lo = clampv(ms_to_units(min_ms), kAdvUnitsMin, kAdvUnitsMax);
    uint32_t hi = clampv(ms_to_units(max_ms), kAdvUnitsMin, kAdvUnitsMax);
    if (hi < lo) hi = lo;

    if ((int32_t)lo == s_adv_itvl_min && (int32_t)hi == s_adv_itvl_max) return ESP_OK;

    s_adv_itvl_min = (int32_t)lo;
    s_adv_itvl_max = (int32_t)hi;

    // Remembered for the next enable if the radio isn't up yet.
    if (!s_running) return ESP_OK;

    // The interval lives in the instance's params, so it can only change via
    // a reconfigure, and reconfigure requires the instance to be stopped.
    bool was_advertising = s_advertising;
    if (was_advertising) {
        int rc = ble_gap_ext_adv_stop(kAdvInstance);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "retune: ext_adv_stop: %d", rc);
        }
        s_advertising = false;
    }

    esp_err_t err = configure_adv_instance();
    if (err != ESP_OK) return err;

    // Reconfigure drops the instance's AD; put the caller's payload back.
    if (s_payload_len) set_adv_payload(s_payload, s_payload_len);

    if (was_advertising) return hal_radio_adv_resume();
    return ESP_OK;
}

esp_err_t hal_radio_set_scan_duty_ms(uint32_t interval_ms, uint32_t window_ms)
{
    if (interval_ms == 0 || window_ms == 0) return ESP_ERR_INVALID_ARG;

    uint16_t itvl = (uint16_t)clampv(ms_to_units(interval_ms),
                                     (uint32_t)kScanUnitsMin, (uint32_t)kScanUnitsMax);
    uint16_t win  = (uint16_t)clampv(ms_to_units(window_ms),
                                     (uint32_t)kScanUnitsMin, (uint32_t)kScanUnitsMax);
    // The controller rejects a window longer than the interval.
    if (win > itvl) win = itvl;

    if (itvl == s_scan_itvl && win == s_scan_window) return ESP_OK;

    s_scan_itvl = itvl;
    s_scan_window = win;

    if (!s_running || !s_scanning) return ESP_OK;

    ble_gap_disc_cancel();
    s_scanning = false;
    return start_scan();
}

void hal_radio_reset_timings()
{
    if (!s_running) {
        // Teardown path (hal_radio_disable clears s_running first) or the
        // radio was never up: just restore the values, nothing to re-apply.
        s_adv_itvl_min = kAdvItvlMin;
        s_adv_itvl_max = kAdvItvlMax;
        s_scan_itvl    = kScanItvl;
        s_scan_window  = kScanWindow;
        return;
    }
    // Live: go through the setters so the controller is actually retuned.
    hal_radio_set_adv_interval_ms(200, 220);
    hal_radio_set_scan_duty_ms(150, 30);
}

void hal_radio_set_recv_cb(RadioRecvCb cb)
{
    std::lock_guard<std::mutex> lk(s_cb_mutex);
    s_recv_cb = std::move(cb);
}

void hal_radio_local_mac(uint8_t out[6])
{
    if (!out) return;
    // s_addr is only populated once the NimBLE host syncs. Callers that run
    // with the radio down — the console's `snapshot`, anything building a
    // payload before hal_radio_enable() — would otherwise get six zero bytes,
    // which is not obviously wrong at the call site but is very wrong by the
    // time it reaches a server using the MAC as an identity key.
    if (!s_host_synced) {
        derive_static_addr(out);
        return;
    }
    memcpy(out, s_addr, 6);
}

static int on_gap_event(struct ble_gap_event *event, void *)
{
    if (event->type != BLE_GAP_EVENT_EXT_DISC) return 0;

    // Advertising data larger than one HCI report (229 bytes) arrives split
    // across several events, and NimBLE does not reassemble. A fragment is not
    // a short frame — it is the front of a longer one — so parsing it would at
    // best drop it on the truncated-AD check below and at worst hand a caller
    // half a payload.
    //
    // Everything this radio sends fits one report by construction
    // (kHalRadioMaxPayload), so this only ever fires for someone else's
    // advertisement or for a regression. Log it, rarely, because the silent
    // version of this cost a full hardware debugging cycle.
    if (event->ext_disc.data_status != BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
        static uint32_t s_frag_count = 0;
        if ((s_frag_count++ % 64) == 0) {
            ESP_LOGW(TAG, "dropped a fragmented advertisement (%u bytes, "
                          "status %u, %lu so far) — over the 229-byte "
                          "single-report limit",
                     (unsigned)event->ext_disc.length_data,
                     (unsigned)event->ext_disc.data_status,
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
            uint16_t cid = (uint16_t)data[p + 2]
                         | ((uint16_t)data[p + 3] << 8);
            if (cid == kCompanyId && flen > 3) {
                const uint8_t *payload = data + p + 4;
                size_t plen = (size_t)flen - 3;

                RadioRecvCb cb;
                {
                    std::lock_guard<std::mutex> lk(s_cb_mutex);
                    cb = s_recv_cb;
                }
                if (cb) cb(event->ext_disc.addr.val, event->ext_disc.rssi,
                           payload, plen);
                break;
            }
        }
        p += 1 + flen;
    }
    return 0;
}
