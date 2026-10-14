#include "app.h"
#include "app_registry.h"

#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_bt.h"
#include "driver/temperature_sensor.h"
#include "lvgl.h"
#include "sdkconfig.h"

#include <cstdio>

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT pin";
    case ESP_RST_SW:         return "SW";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEP_SLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT*";
    case ESP_RST_SDIO:       return "SDIO";
    default:                 return "UNKNOWN";
    }
}

extern "C" const lv_image_dsc_t icon_power;

class PowerApp : public App {
public:
    const char *name() const override { return "Power"; }
    const char *icon() const override { return "PWR"; }
    const char *slug() const override { return "power"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_power; }

    void on_enter(lv_obj_t *root) override
    {
        reset_reason_ = esp_reset_reason();

        esp_chip_info_t ci;
        esp_chip_info(&ci);
        snprintf(chip_str_, sizeof(chip_str_), "%s c%d r%u",
            CONFIG_IDF_TARGET, ci.cores,
            (unsigned)(ci.revision / 100));

        // Temp sensor: ESP32-C3 range 10..50°C is most accurate; pick a wide span.
        temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&tcfg, &temp_) == ESP_OK) {
            temperature_sensor_enable(temp_);
            temp_ok_ = true;
        }

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Power");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        body_ = lv_label_create(root);
        lv_obj_set_style_text_font(body_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(body_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(body_, 8, 30);
        lv_obj_set_width(body_, 304);

        render();
        tick_counter_ = 0;
    }

    void on_exit() override
    {
        body_ = nullptr;
        if (temp_ok_) {
            temperature_sensor_disable(temp_);
            temperature_sensor_uninstall(temp_);
            temp_ok_ = false;
            temp_ = nullptr;
        }
    }

    void on_tick() override
    {
        if (++tick_counter_ < 25) return; // ~500 ms
        tick_counter_ = 0;
        render();
    }

private:
    void render()
    {
        if (!body_) return;

        // Live clocks (DFS would change CPU/APB; on this build PM is off so they're static).
        uint32_t cpu_hz = 0, apb_hz = 0, xtal_hz = 0;
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU,  ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_hz);
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_APB,  ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &apb_hz);
        esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_XTAL, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &xtal_hz);

        esp_bt_controller_status_t bt_state = esp_bt_controller_get_status();
        const char *bt_str =
            bt_state == ESP_BT_CONTROLLER_STATUS_IDLE   ? "idle" :
            bt_state == ESP_BT_CONTROLLER_STATUS_INITED ? "init" :
            bt_state == ESP_BT_CONTROLLER_STATUS_ENABLED ? "on" : "?";

        float tc = 0.0f;
        bool t_ok = temp_ok_ && (temperature_sensor_get_celsius(temp_, &tc) == ESP_OK);

        size_t free_heap = esp_get_free_heap_size();
        size_t min_free  = esp_get_minimum_free_heap_size();
        uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

#ifdef CONFIG_PM_ENABLE
        const char *pm_str = "ENABLED";
#else
        const char *pm_str = "off (no DFS / light-sleep)";
#endif

        char buf[768];
        snprintf(buf, sizeof(buf),
            "reset:  %s\n"
            "chip:   %s\n"
            "CPU:    %u MHz   APB: %u MHz\n"
            "XTAL:   %u MHz\n"
            "PM/DFS: %s\n"
            "BT:     %s\n"
            "TempC:  %s\n"
            "heap:   %u  (min %u)\n"
            "up:     %02u:%02u:%02u\n"
            "----- typ. draw (C3 DS) -----\n"
            "BLE adv ~5 mA  scan ~10 mA\n"
            "modem-sleep ~20 mA\n"
            "light-sleep ~130 uA\n"
            "deep-sleep  ~5 uA",
            reset_reason_str(reset_reason_),
            chip_str_,
            (unsigned)(cpu_hz / 1000000), (unsigned)(apb_hz / 1000000),
            (unsigned)(xtal_hz / 1000000),
            pm_str,
            bt_str,
            t_ok ? (snprintf(tbuf_, sizeof(tbuf_), "%.1f C", tc), tbuf_) : "n/a",
            (unsigned)free_heap, (unsigned)min_free,
            (unsigned)(up_s / 3600), (unsigned)((up_s / 60) % 60), (unsigned)(up_s % 60));

        lv_label_set_text(body_, buf);
    }

    lv_obj_t *body_ = nullptr;
    temperature_sensor_handle_t temp_ = nullptr;
    bool temp_ok_ = false;
    char chip_str_[32] = {};
    char tbuf_[16] = {};
    esp_reset_reason_t reset_reason_ = ESP_RST_UNKNOWN;
    int tick_counter_ = 0;
};

static PowerApp s_instance;
App *app_power_instance() { return &s_instance; }
