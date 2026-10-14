// LCD status screen for the sync station.
//
// Same panel and same graphics system as the badge: ST7789 320x240 over SPI2,
// rendered with LVGL through esp_lvgl_port (the exact stack vendored in the
// badge firmware's managed_components). The layout mirrors the badge Sync
// app — title, bordered panel, status + detail labels, progress bar — so both
// ends of a transfer show the same vocabulary in the same visual language.
//
// Threading: screen_show() diffs under a short-timeout lock and applies
// widget updates under the LVGL port lock, so producers (app_main, the 200 ms
// ticker) never wait on the bus. The display is status-only: if init fails,
// everything here degrades to a no-op and the station keeps working headless.

#include "screen.h"

#include "buttons.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

// Same wiring as the badge (main/pins.h): the station carries the same LCD.
#define LCD_HOST SPI2_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_ROWS 10
#define PIN_LCD_MOSI 10
#define PIN_LCD_CLK  1
#define PIN_LCD_CS   2
#define PIN_LCD_DC   0
#define PIN_LCD_RST  4

// Badge palette (main/apps/ui_theme.h), duplicated: separate IDF projects
// can't share headers.
#define COLOR_BG      0x050505
#define COLOR_PANEL   0x0b0c0b
#define COLOR_BORDER  0x422a43
#define COLOR_ACCENT  0xb087b1
#define COLOR_TEXT    0xf4f4ef
#define COLOR_DIM     0xa0a09a
#define COLOR_GOLD    0xffd166
#define COLOR_MAGENTA 0xff5c8a
#define COLOR_GREEN   0x36e07a

static const char *TAG = "screen";

static SemaphoreHandle_t s_lock;
static bool s_up;   // display + LVGL up; false = headless, calls are no-ops

static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_status;
static lv_obj_t *s_detail;
static lv_obj_t *s_bar;
static lv_obj_t *s_panel_obj;
static lv_obj_t *s_foot;

static char s_line1[SCREEN_LINE_MAX + 1];
static char s_line2[SCREEN_LINE_MAX + 1];
static int  s_recv;
static int  s_total;

static bool disp_init(void)
{
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_LCD_CLK;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_DRAW_ROWS * 2;
    if (spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                 &io_config, &io) != ESP_OK) {
        return false;
    }

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    if (esp_lcd_new_panel_st7789(io, &panel_config, &s_panel) != ESP_OK) {
        return false;
    }

    // Blank a possibly retained frame, then stay hidden until the first
    // screen is fully painted (see screen_init).
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 8192;
    if (lvgl_port_init(&lvgl_cfg) != ESP_OK) {
        return false;
    }

    // Small stripes leave DMA RAM for BLE and heap for the TLS handshake.
    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = io;
    disp_cfg.panel_handle = s_panel;
    disp_cfg.buffer_size = LCD_H_RES * LCD_DRAW_ROWS;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = LCD_H_RES;
    disp_cfg.vres = LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.rotation.swap_xy = true;
    disp_cfg.rotation.mirror_x = true;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.color_format = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma = true;
    disp_cfg.flags.swap_bytes = true;
    if (!lvgl_port_add_disp(&disp_cfg)) {
        return false;
    }
    return true;
}

// Must hold the LVGL port lock.
static void build_chrome(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Sync Station");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_panel_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_panel_obj);
    lv_obj_set_size(s_panel_obj, 280, 132);
    lv_obj_align(s_panel_obj, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(s_panel_obj, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(s_panel_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel_obj, 0, 0);
    lv_obj_set_style_border_width(s_panel_obj, 2, 0);
    lv_obj_set_style_border_color(s_panel_obj, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_clear_flag(s_panel_obj, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(s_panel_obj);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_status, 252);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_20, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -26);

    s_detail = lv_label_create(s_panel_obj);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail, 252);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 8);

    s_bar = lv_bar_create(s_panel_obj);
    lv_obj_set_size(s_bar, 236, 6);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar, 0, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_GOLD), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 0, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);

    s_foot = lv_label_create(scr);
    lv_label_set_text(s_foot, CONFIG_SYNC_STATION_ID);
    lv_obj_set_style_text_color(s_foot, lv_color_hex(COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_foot, &lv_font_montserrat_14, 0);
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// Must hold the LVGL port lock.
static void apply_locked(const char *line1, const char *line2, int recv, int total)
{
    uint32_t border = COLOR_BORDER;
    uint32_t scol = COLOR_TEXT;
    if (strcmp(line1, "DONE") == 0) {
        bool ok = strcmp(line2, "ok") == 0;
        border = scol = ok ? COLOR_GREEN : COLOR_MAGENTA;
    } else if (strcmp(line1, "RX") == 0 || strcmp(line1, "POST") == 0 ||
               strcmp(line1, "SYNC") == 0 || strcmp(line1, "selftest") == 0 ||
               strcmp(line1, "loopback") == 0) {
        border = scol = COLOR_GOLD;
    }
    lv_obj_set_style_border_color(s_panel_obj, lv_color_hex(border), 0);
    lv_label_set_text(s_status, line1);
    lv_obj_set_style_text_color(s_status, lv_color_hex(scol), 0);
    lv_label_set_text(s_detail, line2);

    // Footer always names the live selection, not the flashed default.
    if (s_foot) lv_label_set_text(s_foot, box_id_current());

    if (total > 0) {
        if (recv < 0) recv = 0;
        if (recv > total) recv = total;
        lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_bar, (int32_t)((int64_t)recv * 100 / total), LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    }
}

void screen_init(void)
{
#if !CONFIG_SYNC_SCREEN_ENABLE
    return;
#endif
    s_lock = xSemaphoreCreateMutex();
    s_line1[0] = '\0';
    s_line2[0] = '\0';
    s_recv = 0;
    s_total = 0;

    if (!disp_init()) {
        ESP_LOGW(TAG, "no display, running headless");
        return;
    }
    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "no LVGL lock, running headless");
        return;
    }
    build_chrome();
    apply_locked(CONFIG_SYNC_STATION_ID, "boot", -1, 0);
    // The SPI panel driver drains queued pixel transfers before display-on,
    // so the first visible frame is complete.
    lv_refr_now(NULL);
    esp_lcd_panel_disp_on_off(s_panel, true);
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    ESP_LOGI(TAG, "LVGL heap: free=%u peak_used=%u",
             (unsigned)memory.free_size, (unsigned)memory.max_used);
    lvgl_port_unlock();

    s_up = true;
    ESP_LOGI(TAG, "LCD up (same panel as the badge)");
}

void screen_show(const char *line1, const char *line2, int recv, int total)
{
#if !CONFIG_SYNC_SCREEN_ENABLE
    (void)line1; (void)line2; (void)recv; (void)total;
    return;
#endif
    if (!s_up || !s_lock) return;

    char n1[SCREEN_LINE_MAX + 1], n2[SCREEN_LINE_MAX + 1];
    if (line1) { strncpy(n1, line1, SCREEN_LINE_MAX); n1[SCREEN_LINE_MAX] = '\0'; }
    else n1[0] = '\0';
    if (line2) { strncpy(n2, line2, SCREEN_LINE_MAX); n2[SCREEN_LINE_MAX] = '\0'; }
    else n2[0] = '\0';

    // Lock ordering is always s_lock -> LVGL port lock. The cache updates
    // only after a successful apply, so a lost LVGL race retries on the next
    // tick instead of dropping the frame.
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    bool changed = strcmp(n1, s_line1) != 0 || strcmp(n2, s_line2) != 0 ||
                   recv != s_recv || total != s_total;
    bool applied = false;
    if (changed && lvgl_port_lock(50)) {
        apply_locked(n1, n2, recv, total);
        memcpy(s_line1, n1, sizeof(s_line1));
        memcpy(s_line2, n2, sizeof(s_line2));
        s_recv = recv;
        s_total = total;
        applied = true;
        lvgl_port_unlock();
    }
    (void)applied;
    xSemaphoreGive(s_lock);
}
