#include "hal_accel.h"
#include "hal_i2c.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include <atomic>

static const char *TAG = "hal_accel";

#define SC7A20H_ADDR        0x19
#define I2C_FREQ_HZ         400000

#define REG_WHO_AM_I        0x0F
#define REG_CTRL_REG1       0x20
#define REG_CTRL_REG4       0x23
#define REG_STATUS          0x27
#define REG_OUT_X_L         0x28

#define SC7A20H_CHIP_ID     0x11

// Normal mode with all axes enabled. Bits 7:4 select the output data rate.
#define CTRL_REG1_100_HZ    0x57
#define CTRL_REG1_800_HZ    0x87
// BDU=1, little-endian, ±2 g.
#define CTRL_REG4_VAL       0x80

static i2c_master_dev_handle_t s_dev = nullptr;
static SemaphoreHandle_t s_transaction_mutex = nullptr;
static TaskHandle_t s_cache_task = nullptr;
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static AccelSample s_cache = {};
static bool s_cache_valid = false;
// Written from whichever task enables/disables NFC (under the LVGL lock),
// read by the cache task. Atomic: no mutex needed for a flag.
static std::atomic<bool> s_suspended{false};

// The shared I2C bus also carries the MFRC522, which can wedge SDA/SCL when
// its supply droops on battery power. The old unbounded (-1) transfer
// timeouts wedged this task forever holding the IDF bus lock — and NFC's
// driver install waits that same lock under the LVGL lock, so one bus glitch
// bricked the whole UI with no watchdog to recover (idle current, no
// reboot). Every wait here is therefore bounded, but generously: the timeout
// must cover worst-case scheduling slop (a descheduled task can miss a short
// window even when the transfer itself finished in under a millisecond —
// 50 ms proved too tight at boot). A truly wedged bus still fails in finite
// time and the 20 ms cache period retries on its own; the IDF driver resets
// its own FSM after a failed transaction, so no explicit cross-device reset
// here (those abort each other's transfers). Glitches degrade to retries,
// never to a brick.
static constexpr uint32_t kI2cOpTimeoutMs = 1000;
static constexpr uint32_t kI2cMutexTimeoutMs = 500;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    // No explicit bus reset here: the IDF driver already resets its own FSM
    // on a failed transaction, and resetting from this side would abort the
    // other device's in-flight transfer — both sides resetting on every
    // error is a cross-abort storm in which nothing ever completes.
    return i2c_master_transmit(s_dev, buf, sizeof(buf),
                               pdMS_TO_TICKS(kI2cOpTimeoutMs));
}

static esp_err_t read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    if (len > 1) reg |= 0x80; // auto-increment
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(kI2cOpTimeoutMs));
}

static esp_err_t read_sample(AccelSample *out)
{
    uint8_t status = 0;
    // Few retries: fresh data arrives every 10 ms at 100 Hz ODR and the
    // cache task re-polls every 20 ms anyway. A large count times a long
    // transfer timeout would spin here for a minute on a wedged bus.
    int retries = 10;
    do {
        esp_err_t err = read_regs(REG_STATUS, &status, 1);
        if (err != ESP_OK) return err;
        if (status & 0x08) break; // ZYXDA
        vTaskDelay(pdMS_TO_TICKS(1));
    } while (--retries > 0);
    if (retries == 0) return ESP_ERR_TIMEOUT;

    uint8_t raw[6] = {0};
    esp_err_t err = read_regs(REG_OUT_X_L, raw, 6);
    if (err != ESP_OK) return err;

    auto to_mg = [](uint8_t lo, uint8_t hi) {
        int16_t v = (int16_t)((hi << 8) | lo);
        int16_t counts = v >> 4; // 12-bit left-justified
        return (float)counts * 1.0f; // ±2g → 1 mg/digit
    };

    out->x_mg = to_mg(raw[0], raw[1]);
    out->y_mg = to_mg(raw[2], raw[3]);
    out->z_mg = to_mg(raw[4], raw[5]);
    return ESP_OK;
}

static void cache_store(const AccelSample *sample, bool valid)
{
    portENTER_CRITICAL(&s_cache_lock);
    if (valid) s_cache = *sample;
    s_cache_valid = valid;
    portEXIT_CRITICAL(&s_cache_lock);
}

void hal_accel_set_suspended(bool suspended)
{
    s_suspended.store(suspended, std::memory_order_relaxed);
    // Stale readings must not outlive the suspension: readers see invalid
    // until the resumed task revalidates (within one 20 ms period).
    if (suspended) cache_store(nullptr, false);
}

static void accel_cache_task(void *)
{
    while (true) {
        if (!s_suspended.load(std::memory_order_relaxed)) {
            AccelSample sample = {};
            cache_store(&sample, hal_accel_read(&sample) == ESP_OK);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t hal_accel_init()
{
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = SC7A20H_ADDR;
    dev_cfg.scl_speed_hz = I2C_FREQ_HZ;
    ESP_RETURN_ON_ERROR(
        i2c_master_bus_add_device(hal_i2c_bus(), &dev_cfg, &s_dev),
        TAG, "add device");

    s_transaction_mutex = xSemaphoreCreateMutex();
    if (!s_transaction_mutex) return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_transaction_mutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    uint8_t id = 0;
    err = read_regs(REG_WHO_AM_I, &id, 1);
    if (err == ESP_OK && id != SC7A20H_CHIP_ID) {
        ESP_LOGE(TAG, "unexpected WHO_AM_I 0x%02X", id);
        err = ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SC7A20H detected (0x%02X)", id);
        err = write_reg(REG_CTRL_REG1, CTRL_REG1_100_HZ);
    }
    if (err == ESP_OK) err = write_reg(REG_CTRL_REG4, CTRL_REG4_VAL);
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    xSemaphoreGive(s_transaction_mutex);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "accelerometer setup failed: %s", esp_err_to_name(err));
        return err;
    }

    cache_store(nullptr, false);
    if (xTaskCreate(accel_cache_task, "accel_cache", 3072, nullptr,
                    tskIDLE_PRIORITY + 1, &s_cache_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to start cache task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t hal_accel_set_output_rate(AccelOutputRate rate)
{
    if (!s_dev || !s_transaction_mutex) return ESP_ERR_INVALID_STATE;

    const uint8_t ctrl = rate == AccelOutputRate::Hz800
        ? CTRL_REG1_800_HZ : CTRL_REG1_100_HZ;

    // The background cache task shares this device, so the write and its
    // readback have to be one uninterrupted transaction pair.
    if (xSemaphoreTake(s_transaction_mutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = write_reg(REG_CTRL_REG1, ctrl);
    uint8_t readback = 0;
    if (err == ESP_OK) err = read_regs(REG_CTRL_REG1, &readback, 1);
    xSemaphoreGive(s_transaction_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set ODR failed: %s", esp_err_to_name(err));
        return err;
    }
    if (readback != ctrl) {
        ESP_LOGE(TAG, "ODR readback 0x%02X, expected 0x%02X", readback, ctrl);
        return ESP_ERR_INVALID_RESPONSE;
    }
    // Wait for one sample at the new rate before the next read.
    vTaskDelay(pdMS_TO_TICKS(rate == AccelOutputRate::Hz800 ? 2 : 10));
    return ESP_OK;
}

esp_err_t hal_accel_read(AccelSample *out)
{
    if (!out || !s_dev || !s_transaction_mutex) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_transaction_mutex, pdMS_TO_TICKS(kI2cMutexTimeoutMs)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = read_sample(out);
    xSemaphoreGive(s_transaction_mutex);
    return err;
}

esp_err_t hal_accel_cached_read(AccelSample *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    portENTER_CRITICAL(&s_cache_lock);
    const bool valid = s_cache_valid;
    if (valid) *out = s_cache;
    portEXIT_CRITICAL(&s_cache_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}
