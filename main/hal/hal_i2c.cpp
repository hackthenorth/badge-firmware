#include "hal_i2c.h"
#include "pins.h"
#include "esp_log.h"

static const char *TAG = "hal_i2c";
static i2c_master_bus_handle_t s_bus = nullptr;

esp_err_t hal_i2c_init(void)
{
    if (s_bus) return ESP_OK;

    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
    cfg.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    }
    return err;
}

i2c_master_bus_handle_t hal_i2c_bus(void)
{
    return s_bus;
}
