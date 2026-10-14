// Station-ID editor: the station carries the badge's buttons (74HC165
// shift register, same wiring and shift order). Up opens the ID editor and
// Left/Right step between sync-01/sync-02. The beacon, screen, and Supabase
// heartbeat follow the selection; it persists in NVS across reboots.

#include "buttons.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "screen.h"

// Same pins as the badge (main/pins.h).
#define PIN_HC165_DATA 7
#define PIN_HC165_LOAD 20
#define PIN_HC165_CLK  21
#define SHIFT_DELAY_US 1

// Badge shift order: A B Home Down Left Right Up Aux1, active-low.
#define IDX_DOWN  3
#define IDX_LEFT  4
#define IDX_RIGHT 5
#define IDX_UP    6

#define POLL_MS 20
#define DEBOUNCE_READS 3    // 60 ms stable before an edge counts

#define EDIT_TIMEOUT_US (8 * 1000 * 1000)

static const char *TAG = "buttons";

static const char * const kBoxIds[] = {
    "sync-01",
    "sync-02",
};
#define BOX_COUNT ((int)(sizeof(kBoxIds) / sizeof(kBoxIds[0])))

static int s_index;
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
// The event allowlist shares this prefix across all relay and sync boards.
static const uint8_t kMacPrefix[5] = { 0x24, 0x0A, 0xC4, 0xC8, 0x46 };
#endif
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static void (*s_changed_cb)(void);

// The provisioned last byte follows the station: sync-01 -> B2, sync-02 -> B3.
static uint8_t id_mac_last(void)
{
    return (s_index == 1) ? 0xB3 : 0xB2;
}

typedef enum { M_IDLE, M_ID, M_MACVIEW } edit_mode_t;
static edit_mode_t s_mode = M_IDLE;
static int64_t s_last_press_us;

static uint8_t hc165_read(void)
{
    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 0);
    esp_rom_delay_us(SHIFT_DELAY_US);
    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 1);
    esp_rom_delay_us(SHIFT_DELAY_US);

    uint8_t byte = 0;
    for (int bit = 7; bit >= 0; bit--) {
        if (gpio_get_level((gpio_num_t)PIN_HC165_DATA)) {
            byte |= (uint8_t)(1 << bit);
        }
        gpio_set_level((gpio_num_t)PIN_HC165_CLK, 1);
        esp_rom_delay_us(SHIFT_DELAY_US);
        gpio_set_level((gpio_num_t)PIN_HC165_CLK, 0);
        esp_rom_delay_us(SHIFT_DELAY_US);
    }
    return byte;
}

// Shift position `index` reads low while pressed (see hal_buttons.cpp).
static bool bit_pressed(uint8_t shifted, int index)
{
    return (shifted & (1u << (7 - index))) == 0;
}

const char *box_id_current(void)
{
    return kBoxIds[s_index];
}

void buttons_set_changed_cb(void (*cb)(void))
{
    s_changed_cb = cb;
}

bool box_mac_override(uint8_t out_mac[6])
{
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
    memcpy(out_mac, kMacPrefix, sizeof(kMacPrefix));
    out_mac[5] = id_mac_last();
    return true;
#else
    (void)out_mac;
    return false;
#endif
}

void box_mac_str(const uint8_t mac[6], char out18[18])
{
    snprintf(out18, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void save_index(void)
{
    if (!s_nvs_ok) return;
    if (nvs_set_u8(s_nvs, "box_idx", (uint8_t)s_index) == ESP_OK) {
        nvs_commit(s_nvs);
    }
}

static void show_edit(void)
{
    if (s_mode == M_ID) {
        screen_show("ID >", box_id_current(), -1, 0);
    } else if (s_mode == M_MACVIEW) {
        // Zeroed: with derivation compiled out the override is a no-op and
        // this branch is unreachable, but the compiler can't see that.
        uint8_t mac[6] = { 0 };
        char str[18];
        box_mac_override(mac);
        box_mac_str(mac, str);
        screen_show("MAC", str, -1, 0);
    }
}

static void select_index(int next)
{
    s_index = (next + BOX_COUNT) % BOX_COUNT;
    save_index();
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
    uint8_t mac[6];
    char str[18];
    box_mac_override(mac);
    box_mac_str(mac, str);
    ESP_LOGI(TAG, "station id -> %s (saved, MAC %s after reboot)",
             kBoxIds[s_index], str);
#else
    ESP_LOGI(TAG, "station id -> %s (saved)", kBoxIds[s_index]);
#endif
    sync_beacon_refresh();
    show_edit();
}

// One debounced edge on shift position `index`.
static void on_press(int index)
{
    s_last_press_us = esp_timer_get_time();
    if (index == IDX_UP) {
        if (s_mode == M_ID) {
            s_mode = M_IDLE;
            ESP_LOGI(TAG, "ID edit done");
            if (s_changed_cb) s_changed_cb();
        } else {
            s_mode = M_ID;
            show_edit();
        }
    }
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
    else if (index == IDX_DOWN) {
        // Read-only MAC readout for the whitelist notebook.
        if (s_mode == M_MACVIEW) {
            s_mode = M_IDLE;
            if (s_changed_cb) s_changed_cb();
        } else {
            s_mode = M_MACVIEW;
            show_edit();
        }
    } else if (index == IDX_LEFT) {
#else
    else if (index == IDX_LEFT) {
#endif
        if (s_mode == M_ID) select_index(s_index - 1);
    } else if (index == IDX_RIGHT) {
        if (s_mode == M_ID) select_index(s_index + 1);
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    bool last[8] = { false };
    int stable[8] = { 0 };
    bool fired[8] = { false };
    const int watched[4] = { IDX_UP, IDX_DOWN, IDX_LEFT, IDX_RIGHT };

    for (;;) {
        uint8_t sh = hc165_read();
        bool down_left = false, down_right = false;
        for (int k = 0; k < 4; k++) {
            int i = watched[k];
            bool v = bit_pressed(sh, i);
            if (v == last[i]) {
                if (stable[i] < DEBOUNCE_READS) stable[i]++;
            } else {
                stable[i] = 0;
                last[i] = v;
            }
            bool down = stable[i] >= DEBOUNCE_READS && v;
            if (i == IDX_LEFT) down_left = down;
            if (i == IDX_RIGHT) down_right = down;
            // Left+Right together never steps: one step per deliberate press.
            bool gated = down && !((i == IDX_LEFT && down_right) ||
                                   (i == IDX_RIGHT && down_left));
            if (gated && !fired[i]) {
                fired[i] = true;
                on_press(i);
            } else if (!down) {
                fired[i] = false;
            }
        }

        if (s_mode != M_IDLE &&
            esp_timer_get_time() - s_last_press_us > EDIT_TIMEOUT_US) {
            s_mode = M_IDLE;
            ESP_LOGI(TAG, "edit timed out");
            if (s_changed_cb) s_changed_cb();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void buttons_start(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_HC165_LOAD) | (1ULL << PIN_HC165_CLK),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_HC165_DATA),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 1);
    gpio_set_level((gpio_num_t)PIN_HC165_CLK, 0);

    // NVS selection wins, then the flashed SYNC_STATION_ID.
    s_nvs_ok = (nvs_open("sync_cfg", NVS_READWRITE, &s_nvs) == ESP_OK);
    s_index = 0;
    uint8_t v = 0;
    if (s_nvs_ok && nvs_get_u8(s_nvs, "box_idx", &v) == ESP_OK && v < BOX_COUNT) {
        s_index = v;
    } else {
        for (int i = 0; i < BOX_COUNT; i++) {
            if (strcmp(CONFIG_SYNC_STATION_ID, kBoxIds[i]) == 0) {
                s_index = i;
                break;
            }
        }
    }
#if CONFIG_SYNC_MAC_DERIVE_ENABLE
    uint8_t mac[6];
    char str[18];
    box_mac_override(mac);
    box_mac_str(mac, str);
    ESP_LOGI(TAG, "station id %s, MAC %s (Up: ID, Down: MAC readout)",
             kBoxIds[s_index], str);
#else
    ESP_LOGI(TAG, "station id %s, MAC derive disabled (Up edits ID)",
             kBoxIds[s_index]);
#endif

    xTaskCreate(buttons_task, "buttons", 3072, NULL, 4, NULL);
}
