#include "hal_nfc.h"
#include "hal_accel.h"
#include "rc522_new_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rc522.h"
#include "rc522_picc.h"
#include "picc/rc522_nxp.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <cstring>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "hal_nfc";

#define RC522_I2C_ADDRESS 0x26

// Forward decls for rc522 register-level helpers. These live in
// internal/rc522_pcd_internal.h, which the component exposes only via
// PRIV_INCLUDE_DIRS; the symbols themselves are linker-visible. Same trick
// rc522_new_api.cpp uses to mirror internal types.
extern "C" esp_err_t rc522_pcd_read(rc522_handle_t, uint8_t addr, uint8_t *out);
extern "C" esp_err_t rc522_pcd_set_bits(rc522_handle_t, uint8_t addr, uint8_t bits);
extern "C" esp_err_t rc522_pcd_clear_bits(rc522_handle_t, uint8_t addr, uint8_t bits);

// MFRC522 register addresses + bits used to put the chip in soft power-down.
#define RC522_REG_COMMAND      0x01
#define RC522_REG_VERSION      0x37  // VersionReg: 0x91/0x92 on genuine MFRC522
#define RC522_REG_TX_CONTROL   0x14
#define RC522_BIT_POWER_DOWN   0x10  // CommandReg bit 4
#define RC522_BIT_TX_RF_EN     0x03  // TxControl: Tx1RFEn | Tx2RFEn

// CW driver conductance (datasheet 9.3.3.7/8). Reset defaults are CWGsN=0x8
// (GsNReg 0x27 upper nibble, i.e. 0x80) and CWGsP=0x20 (CWGsPReg 0x28).
// Scaling both codes to ~75% lowers sustained antenna current at some cost
// of range (codes are monotonic in conductance: 0x6/0x18 ≈ 75% of 0x8/0x20).
#define RC522_REG_GSN          0x27
#define RC522_REG_CWGSP        0x28
#define RC522_GSN_REDUCED      0x60  // CWGsN=0x6, ModGsN nibble preserved
#define RC522_CWGSP_REDUCED    0x18  // 24/32 ≈ 75%

static rc522_driver_handle_t s_driver = nullptr;
static rc522_handle_t s_scanner = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;
static bool s_initialized = false;
static bool s_running = false;
// Duty-cycled field: the MFRC522 has no hardware low-power card detect, so
// while enabled we software-cycle the antenna instead of driving the
// 13.56 MHz field continuously. 50% duty (~750 ms on / ~750 ms off) halves
// the dominant (field) current; a tap landing in an off-window just waits
// for the next on-window.
static constexpr uint32_t kFieldOnMs = 750;
static constexpr uint32_t kFieldOffMs = 750;
static bool s_field_on = false;
// True once rc522_start() has run pcd_init. Resumes after that never reset
// the chip (PAUSED→POLLING is a state flip), so the wake path differs.
static bool s_started = false;
static uint32_t s_phase_start_ms = 0;
static bool s_have_card = false;
static NfcCard s_last_card = {};
static rc522_picc_t s_last_picc = {};

static void on_picc_state_changed(void *, esp_event_base_t, int32_t, void *data)
{
    auto *event = (rc522_picc_state_changed_event_t *)data;
    if (!event || !event->picc) return;
    if (event->picc->state != RC522_PICC_STATE_ACTIVE) return;

    NfcCard card = {};
    card.uid_len = event->picc->uid.length;
    if (card.uid_len > sizeof(card.uid)) card.uid_len = sizeof(card.uid);
    memcpy(card.uid, event->picc->uid.value, card.uid_len);
    card.sak = event->picc->sak;
    card.atqa = event->picc->atqa.source;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_card = card;
    s_last_picc = *event->picc;
    s_have_card = true;
    xSemaphoreGive(s_mutex);
}

static esp_err_t hal_nfc_init_once()
{
    // Suppress noisy rc522_picc_send() errors logged on transient PICC read failures
    esp_log_level_set("rc522", ESP_LOG_NONE);

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    hal_nfc_journal("init-mutex-ok");

    esp_err_t err = rc522_new_api_create(RC522_I2C_ADDRESS, &s_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rc522_new_api_create: %s", esp_err_to_name(err));
        return err;
    }
    hal_nfc_journal("init-driver-new-ok");

    err = rc522_driver_install(s_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rc522_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    hal_nfc_journal("init-driver-install-ok");

    rc522_config_t cfg = {};
    cfg.driver = s_driver;
    err = rc522_create(&cfg, &s_scanner);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rc522_create: %s", esp_err_to_name(err));
        return err;
    }
    hal_nfc_journal("init-scanner-create-ok");

    rc522_register_events(s_scanner, RC522_EVENT_PICC_STATE_CHANGED, on_picc_state_changed, nullptr);
    hal_nfc_journal("init-reg-ev-ok");
    s_initialized = true;
    return ESP_OK;
}

static uint32_t now_ms()
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// Crash journal: one flushed LittleFS line per NFC enable stage. RAM logs
// are lost in a battery crash, but this file survives the power-cycle users
// need to recover, so its last line shows exactly which stage died. Read it
// with the console `cat` command after recovering on USB power.
static const char *kJournalPath = "/littlefs/nfc_journal.txt";

void hal_nfc_journal(const char *stage)
{
    if (!stage) return;
    FILE *f = fopen(kJournalPath, "a");
    if (!f) return;
    fprintf(f, "%lu %s\n", (unsigned long)now_ms(), stage);
    fclose(f); // close flushes through LittleFS: power-loss safe per line
}

// Antenna off + chip to soft power-down, ignoring errors. Used when a start
// fails after TX may have been enabled: fail dark, never bright, so a dead
// chip can't pin the rail down while the HAL believes the field is off.
static void power_down_reader()
{
    rc522_pcd_clear_bits(s_scanner, RC522_REG_TX_CONTROL, RC522_BIT_TX_RF_EN);
    rc522_pcd_set_bits(s_scanner, RC522_REG_COMMAND, RC522_BIT_POWER_DOWN);
}

// Best-effort reduced drive. Returns deliberately ignored — a missed write
// only costs range for this on-window, never startup. Clear-then-set
// preserves the reserved and ModGsN bits.
static void apply_reduced_drive()
{
    rc522_pcd_clear_bits(s_scanner, RC522_REG_GSN, 0xF0);
    rc522_pcd_set_bits(s_scanner, RC522_REG_GSN, RC522_GSN_REDUCED);
    rc522_pcd_clear_bits(s_scanner, RC522_REG_CWGSP, 0x3F);
    rc522_pcd_set_bits(s_scanner, RC522_REG_CWGSP, RC522_CWGSP_REDUCED);
}

// Energize the antenna and (re)start polling. Resuming from PAUSED is
// instant (no PCD reset); only the first-ever start pays full init.
//
// Battery shaping: oscillator-start and TX-enable are the two inrush edges.
// They must never stack back-to-back — on a high-impedance source (AA
// alkalines) one combined transient collapses the rail, the CPU dies with no
// reboot, and every conductance trim afterward is irrelevant.
static esp_err_t field_up()
{
    if (s_field_on) return ESP_OK;
    hal_nfc_journal("wake-begin");
    // Wake the chip if field_down() parked it in soft power-down.
    rc522_pcd_clear_bits(s_scanner, RC522_REG_COMMAND, RC522_BIT_POWER_DOWN);
    // Datasheet: PowerDown bit clears itself once the oscillator is stable.
    for (int i = 0; i < 10; ++i) {
        uint8_t v = 0;
        if (rc522_pcd_read(s_scanner, RC522_REG_COMMAND, &v) == ESP_OK
            && (v & RC522_BIT_POWER_DOWN) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    hal_nfc_journal("wake-stable");

    if (!s_started) {
        // First start: rc522_start() resets the chip (erasing any drive
        // programming) and enables TX itself inside pcd_init. Pre-enabling
        // TX here would only add a pointless extra on/off edge, so the
        // single edge left is the one inside pcd_init.
        hal_nfc_journal("start-begin");
        esp_err_t err = rc522_start(s_scanner);
        if (err != ESP_OK) {
            hal_nfc_journal("start-fail");
            ESP_LOGE(TAG, "rc522_start: %s", esp_err_to_name(err));
            power_down_reader();
            return err;
        }
        hal_nfc_journal("start-ok");
        apply_reduced_drive();
        hal_nfc_journal("drive-ok");
        s_started = true;
    } else {
        // Resume: no reset involved, so program reduced drive BEFORE the
        // single TX edge, with a settle gap after the oscillator start so
        // the two transients land separately.
        vTaskDelay(pdMS_TO_TICKS(50));
        apply_reduced_drive();
        rc522_pcd_set_bits(s_scanner, RC522_REG_TX_CONTROL, RC522_BIT_TX_RF_EN);
        // No per-resume journals here: wake-begin/wake-stable above already
        // bracket steady-state cycles, and each line costs a flash write
        // under the LVGL lock every 2 s.
        esp_err_t err = rc522_start(s_scanner); // PAUSED→POLLING state flip
        if (err != ESP_OK) {
            hal_nfc_journal("resume-fail");
            ESP_LOGE(TAG, "rc522_start: %s", esp_err_to_name(err));
            power_down_reader();
            return err;
        }
    }
    s_field_on = true;
    s_phase_start_ms = now_ms();
    return ESP_OK;
}

static void field_down()
{
    if (!s_field_on) return;
    s_field_on = false;
    s_phase_start_ms = now_ms();
    esp_err_t err = rc522_pause(s_scanner);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rc522_pause: %s", esp_err_to_name(err));
    }
    // rc522_pause only stops the polling task. Cut the antenna driver and
    // put the chip in soft power-down so it actually stops drawing current.
    power_down_reader();
}

// Advances the on/off cycle. Driven by hal_nfc_last_card(), which every NFC
// consumer already polls at ~50 Hz — no extra task or timer needed. While a
// card is latched the field is held on so the pending hal_nfc_read_text()
// never races an off-transition; clearing the latch resumes cycling with a
// fresh on-window.
static void duty_tick(bool have_card)
{
    if (!s_running) return;
    if (have_card) {
        if (!s_field_on) field_up();
        s_phase_start_ms = now_ms();
        return;
    }
    uint32_t elapsed = now_ms() - s_phase_start_ms;
    if (s_field_on && elapsed >= kFieldOnMs) {
        field_down();
    } else if (!s_field_on && elapsed >= kFieldOffMs) {
        // On failure, restart the off-window instead of retrying every tick.
        if (field_up() != ESP_OK) s_phase_start_ms = now_ms();
    }
}

esp_err_t hal_nfc_enable()
{
    // Bound the journal so normal use never grows it without limit.
    struct stat st;
    if (stat(kJournalPath, &st) == 0 && st.st_size > 4096)
        unlink(kJournalPath);
    hal_nfc_journal("enable");
    // Suspend accel polling BEFORE touching the bus: the cache task's
    // transfers contend for the IDF bus lock, whose take inside
    // i2c_master_bus_add_device() below is infinite (IDF code, not ours).
    // With polling stopped, at most one already in-flight transfer (itself
    // time-bounded) can delay install. Resumed on failure and on disable.
    hal_accel_set_suspended(true);
    if (!s_initialized) {
        esp_err_t err = hal_nfc_init_once();
        if (err != ESP_OK) {
            hal_nfc_journal("init-fail");
            hal_accel_set_suspended(false);
            return err;
        }
        hal_nfc_journal("init-ok");
    }
    if (s_running) {
        // Re-enable while cycling: bring the field up now so the tap UX is
        // instant instead of waiting out the remainder of an off-window.
        if (field_up() != ESP_OK) {
            hal_nfc_journal("field-up-fail");
            return ESP_FAIL;
        }
        hal_nfc_journal("field-up-ok");
        return ESP_OK;
    }

    if (field_up() != ESP_OK) {
        hal_nfc_journal("field-up-fail");
        hal_accel_set_suspended(false);
        return ESP_FAIL;
    }
    hal_nfc_journal("field-up-ok");
    s_running = true;
    return ESP_OK;
}

esp_err_t hal_nfc_probe(uint8_t *version_out)
{
    if (!s_initialized) {
        esp_err_t err = hal_nfc_init_once();
        if (err != ESP_OK) return err;
    }
    uint8_t v = 0;
    esp_err_t err = rc522_pcd_read(s_scanner, RC522_REG_VERSION, &v);
    if (err != ESP_OK) return err;
    if (version_out) *version_out = v;
    // A floating/stuck bus reads back all-zero or all-one — no chip there.
    if (v == 0x00 || v == 0xFF) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

void hal_nfc_disable()
{
    if (!s_running) return;
    field_down();
    s_running = false;
    hal_accel_set_suspended(false);
}

bool hal_nfc_is_enabled()
{
    return s_running;
}

void hal_nfc_field_down()
{
    if (!s_running) return;
    field_down();
}

bool hal_nfc_last_card(NfcCard *out)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool have = s_have_card;
    xSemaphoreGive(s_mutex);
    duty_tick(have);
    if (!have) return false;
    if (out) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        *out = s_last_card;
        xSemaphoreGive(s_mutex);
    }
    return true;
}

void hal_nfc_clear_last_card()
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_have_card = false;
    xSemaphoreGive(s_mutex);
}

// Walks an NTAG/Ultralight TLV stream and copies the first NDEF Text record
// payload into `out`. Returns ESP_OK on success, ESP_ERR_NOT_FOUND otherwise.
static esp_err_t parse_ndef_text(const uint8_t *buf, size_t buf_len, char *out, size_t out_sz)
{
    size_t pos = 0;
    while (pos < buf_len) {
        uint8_t tag = buf[pos++];
        if (tag == 0x00) continue;       // NULL TLV
        if (tag == 0xFE) break;          // Terminator TLV
        if (pos >= buf_len) return ESP_ERR_NOT_FOUND;

        size_t tlv_len;
        if (buf[pos] == 0xFF) {
            if (pos + 3 > buf_len) return ESP_ERR_NOT_FOUND;
            tlv_len = ((size_t)buf[pos + 1] << 8) | buf[pos + 2];
            pos += 3;
        } else {
            tlv_len = buf[pos++];
        }
        if (pos + tlv_len > buf_len) tlv_len = buf_len - pos;

        if (tag != 0x03) { pos += tlv_len; continue; }

        // Parse first NDEF record in the message.
        size_t mp = pos;
        size_t mend = pos + tlv_len;
        if (mp + 3 > mend) return ESP_ERR_NOT_FOUND;
        uint8_t hdr = buf[mp++];
        uint8_t tnf = hdr & 0x07;
        bool sr = (hdr & 0x10) != 0;
        bool il = (hdr & 0x08) != 0;
        uint8_t type_len = buf[mp++];
        size_t payload_len;
        if (sr) {
            payload_len = buf[mp++];
        } else {
            if (mp + 4 > mend) return ESP_ERR_NOT_FOUND;
            payload_len = ((size_t)buf[mp] << 24) | ((size_t)buf[mp + 1] << 16)
                        | ((size_t)buf[mp + 2] << 8) | buf[mp + 3];
            mp += 4;
        }
        uint8_t id_len = 0;
        if (il) {
            if (mp >= mend) return ESP_ERR_NOT_FOUND;
            id_len = buf[mp++];
        }
        if (mp + type_len + id_len + payload_len > mend) return ESP_ERR_NOT_FOUND;
        if (tnf != 0x01 || type_len != 1 || buf[mp] != 'T') return ESP_ERR_NOT_FOUND;
        mp += type_len + id_len;

        if (payload_len < 1) return ESP_ERR_NOT_FOUND;
        uint8_t status = buf[mp++]; payload_len--;
        uint8_t lang_len = status & 0x3F;
        if (lang_len > payload_len) return ESP_ERR_NOT_FOUND;
        mp += lang_len; payload_len -= lang_len;

        size_t copy = payload_len < out_sz - 1 ? payload_len : out_sz - 1;
        memcpy(out, &buf[mp], copy);
        out[copy] = '\0';
        while (copy > 0) {
            char c = out[copy - 1];
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t') break;
            out[--copy] = '\0';
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t hal_nfc_read_text(char *out, size_t out_sz)
{
    if (!out || out_sz < 16) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!s_initialized || !s_scanner) return ESP_ERR_INVALID_STATE;

    rc522_picc_t picc;
    bool have;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    have = s_have_card;
    if (have) picc = s_last_picc;
    xSemaphoreGive(s_mutex);
    if (!have) return ESP_ERR_NOT_FOUND;

    // The latch normally implies the field is up (duty_tick() holds it while
    // latched), but a caller that reads without polling first could catch an
    // off-window — make sure the transceive below has a live field.
    if (s_running && !s_field_on) {
        if (field_up() != ESP_OK) return ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Read up to 4 chunks (= 16 pages = 64 bytes) from user memory at page 4.
    // Each rc522_nxp_read returns 16 bytes (4 pages).
    uint8_t buf[64] = {};
    size_t valid = 0;
    for (int i = 0; i < 4; i++) {
        esp_err_t err = rc522_nxp_read(s_scanner, &picc, (uint8_t)(4 + i * 4), buf + i * 16);
        if (err != ESP_OK) {
            if (i == 0) return err;
            break;
        }
        valid = (i + 1) * 16;
    }
    return parse_ndef_text(buf, valid, out, out_sz);
}
