// Custom rc522_driver_t vtable that uses the new ESP-IDF i2c_master API
// over hal_i2c's shared bus. The abobija/rc522 component ships only a
// legacy-API i2c driver (via i2c_driver_install), which cannot coexist
// with a new-API bus on the same port. The internal vtable structure is
// mirrored here because the component exposes it only through
// PRIV_INCLUDE_DIRS.

#include "rc522_new_api.h"
#include "hal_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <cstdlib>
#include <cstring>

static const char *TAG = "rc522_new_api";

// Must match internal/rc522_types_internal.h.
typedef struct {
    uint8_t *ptr;
    uint8_t length;
} rc522_bytes_t;

// Must match internal/rc522_driver_internal.h.
struct rc522_driver_handle {
    void *config;
    void *device;
    void *meta;
    esp_err_t (*install)(rc522_driver_handle_t driver);
    esp_err_t (*send)(rc522_driver_handle_t driver, uint8_t address, const rc522_bytes_t *bytes);
    esp_err_t (*receive)(rc522_driver_handle_t driver, uint8_t address, rc522_bytes_t *bytes);
    esp_err_t (*reset)(rc522_driver_handle_t driver);
    esp_err_t (*uninstall)(rc522_driver_handle_t driver);
};

struct NewApiConfig {
    uint8_t device_address;
};

static esp_err_t drv_install(rc522_driver_handle_t driver)
{
    if (!driver || !driver->config) return ESP_ERR_INVALID_ARG;
    auto *cfg = (NewApiConfig *)driver->config;

    i2c_master_bus_handle_t bus = hal_i2c_bus();
    if (!bus) {
        ESP_LOGE(TAG, "hal_i2c bus not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = cfg->device_address;
    dev_cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
        return err;
    }
    driver->device = dev;
    return ESP_OK;
}

static esp_err_t drv_send(rc522_driver_handle_t driver, uint8_t address, const rc522_bytes_t *bytes)
{
    if (!driver || !driver->device || !bytes || !bytes->ptr) return ESP_ERR_INVALID_ARG;
    auto dev = (i2c_master_dev_handle_t)driver->device;

    uint8_t buf[64];
    if ((size_t)bytes->length + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = address;
    memcpy(buf + 1, bytes->ptr, bytes->length);
    // Bounded wait (never -1): on a shared bus, an unbounded transfer that
    // wedges holds the IDF bus lock forever and freezes every other waiter
    // (see hal_accel.cpp). No explicit bus reset: the driver resets its own
    // FSM on failure, and cross-device resets abort each other's transfers.
    return i2c_master_transmit(dev, buf, bytes->length + 1, pdMS_TO_TICKS(1000));
}

static esp_err_t drv_receive(rc522_driver_handle_t driver, uint8_t address, rc522_bytes_t *bytes)
{
    if (!driver || !driver->device || !bytes || !bytes->ptr) return ESP_ERR_INVALID_ARG;
    auto dev = (i2c_master_dev_handle_t)driver->device;
    return i2c_master_transmit_receive(dev, &address, 1, bytes->ptr, bytes->length, pdMS_TO_TICKS(1000));
}

static esp_err_t drv_reset(rc522_driver_handle_t)
{
    // Soft-reset only; no RST pin wired. rc522 library falls back to a
    // PCD soft-reset command when this returns RC522_ERR_RST_PIN_UNUSED.
    return 0xF522 + 12; // RC522_ERR_RST_PIN_UNUSED
}

static esp_err_t drv_uninstall(rc522_driver_handle_t driver)
{
    if (!driver) return ESP_ERR_INVALID_ARG;
    if (driver->device) {
        i2c_master_bus_rm_device((i2c_master_dev_handle_t)driver->device);
        driver->device = nullptr;
    }
    return ESP_OK;
}

esp_err_t rc522_new_api_create(uint8_t device_address, rc522_driver_handle_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    auto *h = (rc522_driver_handle_t)calloc(1, sizeof(struct rc522_driver_handle));
    if (!h) return ESP_ERR_NO_MEM;

    auto *cfg = (NewApiConfig *)calloc(1, sizeof(NewApiConfig));
    if (!cfg) { free(h); return ESP_ERR_NO_MEM; }
    cfg->device_address = device_address;

    h->config = cfg;
    h->device = nullptr;
    h->meta = nullptr;
    h->install = drv_install;
    h->send = drv_send;
    h->receive = drv_receive;
    h->reset = drv_reset;
    h->uninstall = drv_uninstall;

    *out = h;
    return ESP_OK;
}
