// Station-ID + STA-MAC editor: the relay carries the badge's buttons
// (74HC165 shift register, same wiring and shift order). Up opens the
// station-ID editor, Down the MAC editor; Left/Right adjust the open row.
// The beacon and the Supabase heartbeat follow ID changes immediately; a
// MAC change is NVS-persisted and takes effect after a reboot, because the
// WiFi MAC can only be set before the interface starts.

#include "buttons.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_mac.h"
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

// Event allowlist: 24:0A:C4:C8:46:A0 through 24:0A:C4:C8:46:B3.
// Every board must use this prefix, not its own factory MAC prefix.
#if CONFIG_RELAY_MAC_EDIT_ENABLE
static const uint8_t kMacPrefix[5] = { 0x24, 0x0A, 0xC4, 0xC8, 0x46 };
#endif
#define MAC_FIRST 0xA0
#define MAC_LAST  0xB3

#define EDIT_TIMEOUT_US (8 * 1000 * 1000)

static const char *TAG = "buttons";

static const char * const kBoxIds[] = {
    "solana-booth",
    "hardware-hub",
    "extended-bay",
    "mentor-cafe",
    "third-floor",
    "fourth-floor",
    "fifth-floor",
    "seventh-floor",
    "solana-booth-final",
};
#define BOX_COUNT ((int)(sizeof(kBoxIds) / sizeof(kBoxIds[0])))

static int s_index;
#if CONFIG_RELAY_MAC_EDIT_ENABLE
static bool s_mac_set;
static uint8_t s_mac_last;
#endif
static uint8_t s_base_mac[6];
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static void (*s_changed_cb)(void);

typedef enum { M_IDLE, M_ID, M_MAC } edit_mode_t;
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

void box_base_mac(uint8_t out_mac[6])
{
    memcpy(out_mac, s_base_mac, 6);
}

bool box_mac_override(uint8_t out_mac[6])
{
#if CONFIG_RELAY_MAC_EDIT_ENABLE
    if (!s_mac_set) return false;
    memcpy(out_mac, kMacPrefix, sizeof(kMacPrefix));
    out_mac[5] = s_mac_last;
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

static void save_u8(const char *key, uint8_t v)
{
    if (!s_nvs_ok) return;
    if (nvs_set_u8(s_nvs, key, v) == ESP_OK) nvs_commit(s_nvs);
}

// Paint the open editor. Runs on the button task; screen_show() takes the
// display lock itself like the ticker's calls do.
static void show_edit(void)
{
    if (s_mode == M_ID) {
        screen_show("ID >", box_id_current(), -1, 0);
    } else if (s_mode == M_MAC) {
        // Zeroed: with the editor compiled out the override is a no-op and
        // this branch is unreachable, but the compiler can't see that.
        uint8_t mac[6] = { 0 };
        char str[24];
        if (box_mac_override(mac)) {
            box_mac_str(mac, str);
        } else {
            box_base_mac(mac);
            box_mac_str(mac, str);
            strcat(str, " auto");
        }
        screen_show("MAC >", str, -1, 0);
    }
}

static void select_index(int next)
{
    s_index = (next + BOX_COUNT) % BOX_COUNT;
    save_u8("box_idx", (uint8_t)s_index);
    ESP_LOGI(TAG, "station id -> %s (saved)", kBoxIds[s_index]);
    relay_beacon_refresh();
    show_edit();
}

#if CONFIG_RELAY_MAC_EDIT_ENABLE
static void adjust_mac(int dir)
{
    if (!s_mac_set) {
        // First touch claims the bottom of the provisioned range.
        s_mac_set = true;
        s_mac_last = MAC_FIRST;
    } else {
        int v = (int)s_mac_last + dir;
        if (v < MAC_FIRST) v = MAC_LAST;
        if (v > MAC_LAST) v = MAC_FIRST;
        s_mac_last = (uint8_t)v;
    }
    save_u8("mac_set", 1);
    save_u8("mac_last", s_mac_last);
    uint8_t mac[6];
    char str[18];
    box_mac_override(mac);
    box_mac_str(mac, str);
    ESP_LOGI(TAG, "MAC set %s (saved, reboot to apply)", str);
    show_edit();
}
#endif

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
#if CONFIG_RELAY_MAC_EDIT_ENABLE
    else if (index == IDX_DOWN) {
        if (s_mode == M_MAC) {
            s_mode = M_IDLE;
            ESP_LOGI(TAG, "MAC edit done");
            if (s_changed_cb) s_changed_cb();
        } else {
            s_mode = M_MAC;
            show_edit();
        }
    } else if (index == IDX_LEFT) {
        if (s_mode == M_ID) select_index(s_index - 1);
        else if (s_mode == M_MAC) adjust_mac(-1);
    } else if (index == IDX_RIGHT) {
        if (s_mode == M_ID) select_index(s_index + 1);
        else if (s_mode == M_MAC) adjust_mac(+1);
    }
#else
    else if (index == IDX_LEFT) {
        if (s_mode == M_ID) select_index(s_index - 1);
    } else if (index == IDX_RIGHT) {
        if (s_mode == M_ID) select_index(s_index + 1);
    }
#endif
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

    if (esp_efuse_mac_get_default(s_base_mac) != ESP_OK) {
        memset(s_base_mac, 0, sizeof(s_base_mac));
    }

    s_nvs_ok = (nvs_open("relay_cfg", NVS_READWRITE, &s_nvs) == ESP_OK);

    // Station: NVS selection wins, then the flashed RELAY_BOX_ID.
    s_index = 0;
    uint8_t v = 0;
    if (s_nvs_ok && nvs_get_u8(s_nvs, "box_idx", &v) == ESP_OK && v < BOX_COUNT) {
        s_index = v;
    } else {
        for (int i = 0; i < BOX_COUNT; i++) {
            if (strcmp(CONFIG_RELAY_BOX_ID, kBoxIds[i]) == 0) {
                s_index = i;
                break;
            }
        }
    }

#if CONFIG_RELAY_MAC_EDIT_ENABLE
    // MAC: starts unconfigured (chip default) until the Down editor sets it.
    s_mac_set = false;
    s_mac_last = MAC_FIRST;
    if (s_nvs_ok) {
        uint8_t f = 0, l = 0;
        if (nvs_get_u8(s_nvs, "mac_set", &f) == ESP_OK && f == 1 &&
            nvs_get_u8(s_nvs, "mac_last", &l) == ESP_OK &&
            l >= MAC_FIRST && l <= MAC_LAST) {
            s_mac_set = true;
            s_mac_last = l;
        }
    }

    if (s_mac_set) {
        uint8_t mac[6];
        char str[18];
        box_mac_override(mac);
        box_mac_str(mac, str);
        ESP_LOGI(TAG, "station id %s, NVS MAC %s (Up/Down to edit)",
                 kBoxIds[s_index], str);
    } else {
        ESP_LOGI(TAG, "station id %s, MAC auto (Up/Down to edit)",
                 kBoxIds[s_index]);
    }
#else
    ESP_LOGI(TAG, "station id %s, MAC editor disabled (Up edits ID)",
             kBoxIds[s_index]);
#endif

    xTaskCreate(buttons_task, "buttons", 3072, NULL, 4, NULL);
}
