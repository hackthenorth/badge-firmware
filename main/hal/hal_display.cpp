#include "hal_display.h"
#include "ui_theme.h"
#include "pins.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"

#define LCD_HOST SPI2_HOST
#define LCD_H_RES 320
#define LCD_V_RES 240
#define LCD_DRAW_ROWS 30

static lv_obj_t *s_root = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t *s_disp = nullptr;

void hal_display_init(void)
{
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_LCD_CLK;
    buscfg.max_transfer_sz = LCD_H_RES * LCD_DRAW_ROWS * 2;
    spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = PIN_LCD_CS;
    io_config.dc_gpio_num = PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io);

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = PIN_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    esp_lcd_new_panel_st7789(io, &panel_config, &s_panel);

    // A warm reset can leave the ST7789 showing its old frame. Blank it at
    // the earliest point at which SPI is available, then keep it hidden until
    // hal_display_show() has painted the complete first frame.
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, true, false);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_stack = 16384;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Two 30-row stripes use 38.4 KB of DMA RAM. Inactive Outrun, Sponsors,
    // and Etch working arrays now return about 28 KB to the heap, covering
    // the increase from 10 rows while preserving BLE controller headroom.
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
    s_disp = lvgl_port_add_disp(&disp_cfg);

    if (lvgl_port_lock(0)) {
        s_root = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_root);
        lv_obj_set_size(s_root, LCD_H_RES, LCD_V_RES);
        lv_obj_set_pos(s_root, 0, 0);
        lv_obj_set_style_bg_color(s_root, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
        lvgl_port_unlock();
    }
}

lv_obj_t *hal_display_root(void)
{
    return s_root;
}

lv_display_t *hal_display_lv(void)
{
    return s_disp;
}

void hal_display_show(void)
{
    if (!s_panel || !lvgl_port_lock(0)) return;

    // esp_lcd's SPI parameter path waits for every queued color transfer
    // before sending DISPON. The panel therefore cannot expose a partial or
    // retained frame between this refresh and becoming visible.
    lv_refr_now(nullptr);
    esp_lcd_panel_disp_on_off(s_panel, true);

    lvgl_port_unlock();
}
