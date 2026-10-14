// ESP-IDF and FreeRTOS services the firmware calls directly, re-implemented
// for the browser. Hardware peripherals live in emu_hal.cpp.

#include "emu.h"

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_bt.h"
#include "esp_chip_info.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <emscripten.h>

#include <dirent.h>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef EMU_FW_VERSION
#define EMU_FW_VERSION "emulator"
#endif

// ---- clock -----------------------------------------------------------------

static int64_t s_now_us = 0;

int64_t emu_now_us() { return s_now_us; }
void emu_advance_us(int64_t delta_us)
{
    if (delta_us > 0) s_now_us += delta_us;
}

extern "C" int64_t esp_timer_get_time(void) { return s_now_us; }

extern "C" void vTaskDelay(TickType_t ticks)
{
    emu_advance_us((int64_t)ticks * portTICK_PERIOD_MS * 1000);
}

extern "C" TickType_t xTaskGetTickCount(void)
{
    return (TickType_t)(s_now_us / (portTICK_PERIOD_MS * 1000));
}

// The firmware treats these as "how close is this task to overflowing".
// wasm has a 1 MB stack, so report a comfortable margin.
extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 8192; }
extern "C" UBaseType_t uxTaskGetNumberOfTasks(void) { return 1; }
extern "C" UBaseType_t uxTaskGetSystemState(TaskStatus_t *out, UBaseType_t capacity,
                                            uint32_t *total_run_time)
{
    if (total_run_time) *total_run_time = 0;
    if (!out || capacity == 0) return 0;
    out[0] = { "emulator", 1, 8192 };
    return 1;
}

struct EmuSemaphore {
    int unused;
};

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return static_cast<SemaphoreHandle_t>(calloc(1, sizeof(EmuSemaphore)));
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t sem) { free(sem); }

// ---- errors, logging, restart ----------------------------------------------

extern "C" const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:                   return "ESP_OK";
    case ESP_FAIL:                 return "ESP_FAIL";
    case ESP_ERR_NO_MEM:           return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:      return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:    return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:     return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:        return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED:    return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT:          return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC:      return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_NVS_NOT_FOUND:    return "ESP_ERR_NVS_NOT_FOUND";
    default:                       return "ERROR";
    }
}

EM_JS(void, emu_js_halt, (int reason, const char *message), {
    const error = new Error(message ? UTF8ToString(message) : "halt");
    error.emuHalt = { reason: reason };
    throw error;
});

[[noreturn]] void emu_halt(int reset_reason, const char *message)
{
    fflush(stdout);
    emu_js_halt(reset_reason, message);
    __builtin_unreachable();
}

extern "C" void emu_error_check_failed(esp_err_t code, const char *file, int line,
                                       const char *expr)
{
    printf("ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s) at %s:%d\nexpression: %s\n",
           (unsigned)code, esp_err_to_name(code), file, line, expr);
    emu_halt(ESP_RST_PANIC, "ESP_ERROR_CHECK failed");
}

extern "C" void esp_restart(void)
{
    emu_halt(ESP_RST_SW, "esp_restart");
}

extern "C" esp_reset_reason_t esp_reset_reason(void)
{
    return static_cast<esp_reset_reason_t>(
        EM_ASM_INT({ return (Module.emu && Module.emu.resetReason) | 0; }));
}

extern "C" void emu_log(esp_log_level_t level, const char *tag, const char *fmt, ...)
{
    static const char kLetters[] = { ' ', 'E', 'W', 'I', 'D', 'V' };
    if (level <= ESP_LOG_NONE || level > ESP_LOG_VERBOSE) return;
    // Matches the firmware's default: INFO and above reach the console.
    if (level > ESP_LOG_INFO) return;
    char body[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    printf("%c (%lld) %s: %s\n", kLetters[level],
           (long long)(s_now_us / 1000), tag ? tag : "?", body);
}

// ---- entropy and checksums -------------------------------------------------

extern "C" void esp_fill_random(void *buf, size_t len)
{
    EM_ASM({
        const view = HEAPU8.subarray($0, $0 + $1);
        // getRandomValues refuses requests over 64 KB.
        for (let off = 0; off < view.length; off += 65536) {
            crypto.getRandomValues(view.subarray(off, Math.min(view.length, off + 65536)));
        }
    }, buf, len);
}

extern "C" uint32_t esp_random(void)
{
    uint32_t value = 0;
    esp_fill_random(&value, sizeof(value));
    return value;
}

// zlib convention, which is what the ROM routine implements.
extern "C" uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (uint32_t i = 0; i < len; ++i) crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// ---- heap ------------------------------------------------------------------

// A badge has roughly 182 KB of DRAM heap after static allocations. The wasm
// heap is not capped to that, but the diagnostics read as they would on a
// badge: real allocation totals measured against the real budget.
static constexpr size_t kHeapTotal = 182 * 1024;
static constexpr size_t kHeapFloor = 4 * 1024;
static size_t s_min_free = kHeapTotal;

static size_t heap_free_now()
{
    static size_t baseline = 0;
    const size_t used = (size_t)mallinfo().uordblks;
    // Whatever the runtime had allocated before app_main is not "the app".
    if (baseline == 0 || used < baseline) baseline = used;
    const size_t app_used = used - baseline;
    const size_t free_bytes =
        app_used + kHeapFloor >= kHeapTotal ? kHeapFloor : kHeapTotal - app_used;
    if (free_bytes < s_min_free) s_min_free = free_bytes;
    return free_bytes;
}

extern "C" void *heap_caps_malloc(size_t size, uint32_t) { return malloc(size); }
extern "C" void heap_caps_free(void *ptr) { free(ptr); }
extern "C" size_t heap_caps_get_total_size(uint32_t) { return kHeapTotal; }
extern "C" size_t heap_caps_get_free_size(uint32_t) { return heap_free_now(); }
extern "C" size_t heap_caps_get_minimum_free_size(uint32_t)
{
    heap_free_now();
    return s_min_free;
}
extern "C" size_t heap_caps_get_largest_free_block(uint32_t)
{
    // No fragmentation model: report most of the free heap as contiguous.
    return heap_free_now() / 4 * 3;
}
extern "C" uint32_t esp_get_free_heap_size(void) { return (uint32_t)heap_free_now(); }
extern "C" uint32_t esp_get_minimum_free_heap_size(void)
{
    heap_free_now();
    return (uint32_t)s_min_free;
}

// ---- chip, flash, partitions -----------------------------------------------

extern "C" const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t desc = {};
    if (!desc.magic_word) {
        desc.magic_word = 0xABCD5432;
        snprintf(desc.version, sizeof(desc.version), "%s", EMU_FW_VERSION);
        snprintf(desc.project_name, sizeof(desc.project_name), "hello_world");
        snprintf(desc.time, sizeof(desc.time), "%s", __TIME__);
        snprintf(desc.date, sizeof(desc.date), "%s", __DATE__);
        snprintf(desc.idf_ver, sizeof(desc.idf_ver), "emulator");
    }
    return &desc;
}

extern "C" void esp_chip_info(esp_chip_info_t *out)
{
    if (!out) return;
    out->model = CHIP_ESP32C3;
    out->features = 0x12; // BLE + 2.4 GHz
    out->revision = 4;    // v0.4
    out->cores = 1;
}

static constexpr uint32_t kFlashBytes = 4 * 1024 * 1024;
static constexpr uint32_t kLittlefsBytes = 0x140000;
// Size of the image `idf.py build` produces today; the Memory app shows it
// against the factory slot.
static constexpr uint32_t kAppImageBytes = 2651840;

extern "C" esp_err_t esp_flash_get_size(esp_flash_t *, uint32_t *out_size)
{
    if (out_size) *out_size = kFlashBytes;
    return ESP_OK;
}

// partitions.csv, with the offsets the IDF partition tool assigns.
static const esp_partition_t kPartitions[] = {
    { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS,      0x9000,   0x4000,   "nvs" },
    { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_PHY,      0xd000,   0x1000,   "phy_init" },
    { ESP_PARTITION_TYPE_APP,  ESP_PARTITION_SUBTYPE_APP_FACTORY,   0x10000,  0x2A0000, "factory" },
    { ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0x2B0000, kLittlefsBytes, "storage" },
};
static constexpr size_t kPartitionCount = sizeof(kPartitions) / sizeof(kPartitions[0]);

// Iterators are 1-based indices so that "end" is a null handle, as in IDF.
extern "C" esp_partition_iterator_t esp_partition_find(esp_partition_type_t,
                                                       esp_partition_subtype_t,
                                                       const char *)
{
    return reinterpret_cast<esp_partition_iterator_t>((uintptr_t)1);
}

extern "C" const esp_partition_t *esp_partition_get(esp_partition_iterator_t it)
{
    const uintptr_t index = reinterpret_cast<uintptr_t>(it);
    return index >= 1 && index <= kPartitionCount ? &kPartitions[index - 1] : nullptr;
}

extern "C" esp_partition_iterator_t esp_partition_next(esp_partition_iterator_t it)
{
    const uintptr_t index = reinterpret_cast<uintptr_t>(it);
    return index >= 1 && index < kPartitionCount
        ? reinterpret_cast<esp_partition_iterator_t>(index + 1) : nullptr;
}

extern "C" void esp_partition_iterator_release(esp_partition_iterator_t) {}

extern "C" const esp_partition_t *esp_ota_get_running_partition(void)
{
    return &kPartitions[2];
}

// Only the Memory app reads flash, and only to size the running image. Serve
// a one-segment image header whose length is the real firmware binary's.
extern "C" esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset,
                                        void *dst, size_t size)
{
    if (!partition || !dst) return ESP_ERR_INVALID_ARG;
    memset(dst, 0xff, size);
    if (partition != &kPartitions[2]) return ESP_OK;

    esp_image_header_t header = {};
    header.magic = ESP_IMAGE_HEADER_MAGIC;
    header.segment_count = 1;
    header.hash_appended = 1;
    esp_image_segment_header_t segment = {};
    // Total = header + segment header + data, padded to 16, + checksum + SHA.
    segment.data_len = kAppImageBytes - sizeof(header) - sizeof(segment) - 33;

    if (offset == 0 && size == sizeof(header)) {
        memcpy(dst, &header, sizeof(header));
    } else if (offset == sizeof(header) && size == sizeof(segment)) {
        memcpy(dst, &segment, sizeof(segment));
    }
    return ESP_OK;
}

static size_t tree_bytes(const char *path)
{
    size_t total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    while (const dirent *entry = readdir(dir)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name)
            >= (int)sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            total += 4096 + tree_bytes(child);
        } else {
            // LittleFS stores small files inline; larger ones take whole blocks.
            total += st.st_size <= 1024 ? 256 : ((size_t)st.st_size + 4095) / 4096 * 4096;
        }
    }
    closedir(dir);
    return total;
}

extern "C" esp_err_t esp_littlefs_info(const char *, size_t *total_bytes, size_t *used_bytes)
{
    if (total_bytes) *total_bytes = kLittlefsBytes;
    if (used_bytes) {
        // Two superblock copies are always in use.
        size_t used = 8192 + tree_bytes("/littlefs");
        *used_bytes = used > kLittlefsBytes ? kLittlefsBytes : used;
    }
    return ESP_OK;
}

extern "C" esp_err_t esp_clk_tree_src_get_freq_hz(soc_module_clk_t clk,
                                                  esp_clk_tree_src_freq_precision_t,
                                                  uint32_t *freq_hz)
{
    if (!freq_hz) return ESP_ERR_INVALID_ARG;
    // CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80 with a 40 MHz crystal.
    *freq_hz = clk == SOC_MOD_CLK_XTAL ? 40000000u : 80000000u;
    return ESP_OK;
}

struct EmuTemperatureSensor {
    int unused;
};

extern "C" esp_err_t temperature_sensor_install(const temperature_sensor_config_t *,
                                                temperature_sensor_handle_t *out)
{
    static EmuTemperatureSensor sensor;
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = &sensor;
    return ESP_OK;
}
extern "C" esp_err_t temperature_sensor_uninstall(temperature_sensor_handle_t) { return ESP_OK; }
extern "C" esp_err_t temperature_sensor_enable(temperature_sensor_handle_t) { return ESP_OK; }
extern "C" esp_err_t temperature_sensor_disable(temperature_sensor_handle_t) { return ESP_OK; }
extern "C" esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t, float *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    // A warm desk-side chip with a slow wobble, so the reading is visibly live.
    *out = 31.5f + 0.5f * (float)((s_now_us / 1000000) % 4);
    return ESP_OK;
}

extern "C" esp_err_t gpio_wakeup_enable(gpio_num_t, gpio_int_type_t) { return ESP_OK; }

// ---- NVS -------------------------------------------------------------------
//
// One file per key under /nvs/<namespace>/. The page persists /nvs alongside
// /littlefs, so a credential survives reboots but not "erase flash".

static constexpr uint32_t kNvsMaxHandles = 8;
static struct {
    bool open;
    bool writable;
    char name_space[32];
} s_nvs[kNvsMaxHandles];

extern "C" esp_err_t nvs_flash_init(void)
{
    mkdir("/nvs", 0775);
    return ESP_OK;
}

static bool nvs_key_path(nvs_handle_t handle, const char *key, char *out, size_t out_sz)
{
    if (handle == 0 || handle > kNvsMaxHandles || !s_nvs[handle - 1].open || !key) return false;
    return snprintf(out, out_sz, "/nvs/%s/%s", s_nvs[handle - 1].name_space, key)
        < (int)out_sz;
}

extern "C" esp_err_t nvs_open(const char *name_space, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (!name_space || !out || strlen(name_space) >= sizeof(s_nvs[0].name_space))
        return ESP_ERR_INVALID_ARG;
    char dir[64];
    snprintf(dir, sizeof(dir), "/nvs/%s", name_space);
    struct stat st;
    const bool exists = stat(dir, &st) == 0;
    if (!exists) {
        if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
        mkdir("/nvs", 0775);
        if (mkdir(dir, 0775) != 0 && errno != EEXIST) return ESP_FAIL;
    }
    for (uint32_t i = 0; i < kNvsMaxHandles; ++i) {
        if (s_nvs[i].open) continue;
        s_nvs[i].open = true;
        s_nvs[i].writable = mode == NVS_READWRITE;
        snprintf(s_nvs[i].name_space, sizeof(s_nvs[i].name_space), "%s", name_space);
        *out = i + 1;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

extern "C" void nvs_close(nvs_handle_t handle)
{
    if (handle >= 1 && handle <= kNvsMaxHandles) s_nvs[handle - 1].open = false;
}

extern "C" esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out,
                                  size_t *length)
{
    char path[128];
    if (!length) return ESP_ERR_INVALID_ARG;
    if (!nvs_key_path(handle, key, path, sizeof(path))) return ESP_ERR_NVS_INVALID_HANDLE;
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NVS_NOT_FOUND;
    const size_t stored = (size_t)st.st_size;
    if (!out) {
        *length = stored;
        return ESP_OK;
    }
    if (*length < stored) {
        *length = stored;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NVS_NOT_FOUND;
    const size_t got = fread(out, 1, stored, f);
    fclose(f);
    *length = got;
    return got == stored ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value,
                                  size_t length)
{
    char path[128];
    if (!value) return ESP_ERR_INVALID_ARG;
    if (!nvs_key_path(handle, key, path, sizeof(path))) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!s_nvs[handle - 1].writable) return ESP_ERR_NVS_READ_ONLY;
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    const bool ok = fwrite(value, 1, length, f) == length;
    fclose(f);
    return ok ? ESP_OK : ESP_FAIL;
}

extern "C" esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    char path[128];
    if (!nvs_key_path(handle, key, path, sizeof(path))) return ESP_ERR_NVS_INVALID_HANDLE;
    if (!s_nvs[handle - 1].writable) return ESP_ERR_NVS_READ_ONLY;
    return unlink(path) == 0 ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

extern "C" esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

// libsodium calls this on API misuse; core.c (which defines it) drags in every
// primitive's runtime dispatch, so the emulator provides it directly.
extern "C" void sodium_misuse(void)
{
    printf("sodium_misuse\n");
    emu_halt(ESP_RST_PANIC, "sodium_misuse");
}
