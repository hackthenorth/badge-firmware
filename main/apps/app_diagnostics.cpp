#include "app.h"
#include "app_registry.h"
#include "hal_accel.h"
#include "hal_nfc.h"
#include "hal_config.h"
#include "script_app.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <cstdio>

extern "C" const lv_image_dsc_t icon_diagnostics;

class DiagnosticsApp : public App {
public:
    const char *name() const override { return "Diag"; }
    const char *icon() const override { return "DBG"; }
    const char *slug() const override { return "diagnostics"; }
    AppAccess access() const override { return AppAccess::Internal; }
    const lv_image_dsc_t *icon_image() const override { return &icon_diagnostics; }

    void on_enter(lv_obj_t *root) override
    {
        // Diagnostics surfaces NFC tag-seen state; enable while open.
        hal_nfc_enable();

        int boots = hal_config_get_int("system", "boots", 0) + 1;
        hal_config_set_int("system", "boots", boots);
        boot_count_ = boots;

        // Cache static info once.
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        chip_cores_ = ci.cores;
        chip_rev_ = ci.revision;
        const char *tgt = CONFIG_IDF_TARGET;
        snprintf(chip_model_, sizeof(chip_model_), "%s rev%u.%u",
            tgt,
            (unsigned)(ci.revision / 100),
            (unsigned)(ci.revision % 100));

        uint32_t fs = 0;
        esp_flash_get_size(nullptr, &fs);
        flash_mb_ = fs / (1024 * 1024);

        AccelSample s;
        accel_ok_ = (hal_accel_read(&s) == ESP_OK);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Diagnostics");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

        body_ = lv_label_create(root);
        lv_obj_set_style_text_font(body_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(body_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(body_, 16, 36);
        lv_obj_set_width(body_, 290);

        render();
        tick_counter_ = 0;
    }

    void on_exit() override
    {
        body_ = nullptr;
    }

    void on_tick() override
    {
        // Roughly every 500 ms (on_tick fires every 20 ms).
        if (++tick_counter_ < 25) return;
        tick_counter_ = 0;
        render();
    }

private:
    void render()
    {
        if (!body_) return;

        size_t free_heap = esp_get_free_heap_size();
        size_t min_free  = esp_get_minimum_free_heap_size();
        uint64_t uptime_us = esp_timer_get_time();
        uint32_t uptime_s = (uint32_t)(uptime_us / 1000000ULL);

        NfcCard card;
        bool nfc_seen = hal_nfc_last_card(&card);

        // Last Lua run's heap, via the C++ twin of badge.sys.stats().
        char lua_line[96] = {};
        ScriptAppLuaStats lua_stats = {};
        if (script_app_last_lua_stats(&lua_stats)) {
            snprintf(lua_line, sizeof(lua_line), "%s %u/%u KiB peak %u",
                     lua_stats.slug,
                     (unsigned)(lua_stats.used / 1024),
                     (unsigned)(lua_stats.limit / 1024),
                     (unsigned)(lua_stats.peak / 1024));
        }

        char buf[640];
        snprintf(buf, sizeof(buf),
            "chip:   %s\n"
            "cores:  %d\n"
            "flash:  %u MB\n"
            "heap:   %u (min %u)\n"
            "uptime: %02u:%02u:%02u\n"
            "boots:  %d\n"
            "accel:  %s\n"
            "nfc:    %s%s%s",
            chip_model_,
            chip_cores_,
            (unsigned)flash_mb_,
            (unsigned)free_heap, (unsigned)min_free,
            (unsigned)(uptime_s / 3600), (unsigned)((uptime_s / 60) % 60), (unsigned)(uptime_s % 60),
            boot_count_,
            accel_ok_ ? "OK (0x11)" : "FAIL",
            nfc_seen ? "tag seen" : "waiting",
            lua_line[0] ? "\nlua:    " : "",
            lua_line[0] ? lua_line : "");

        lv_label_set_text(body_, buf);
    }

    lv_obj_t *body_ = nullptr;
    char chip_model_[48] = {};
    int chip_cores_ = 0;
    int chip_rev_ = 0;
    int flash_mb_ = 0;
    bool accel_ok_ = false;
    int boot_count_ = 0;
    int tick_counter_ = 0;
};

static DiagnosticsApp s_instance;
App *app_diagnostics_instance() { return &s_instance; }
