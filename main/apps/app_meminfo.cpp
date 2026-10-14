#include "app.h"
#include "app_registry.h"

extern "C" const lv_image_dsc_t icon_meminfo;

#include "esp_app_format.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

const char *subtype_label(esp_partition_type_t t, esp_partition_subtype_t s)
{
    if (t == ESP_PARTITION_TYPE_APP) {
        if (s == ESP_PARTITION_SUBTYPE_APP_FACTORY) return "app/factory";
        if (s >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
            && s <  ESP_PARTITION_SUBTYPE_APP_OTA_MAX) return "app/ota";
        if (s == ESP_PARTITION_SUBTYPE_APP_TEST) return "app/test";
        return "app";
    }
    switch (s) {
    case ESP_PARTITION_SUBTYPE_DATA_OTA:      return "data/ota";
    case ESP_PARTITION_SUBTYPE_DATA_PHY:      return "data/phy";
    case ESP_PARTITION_SUBTYPE_DATA_NVS:      return "data/nvs";
    case ESP_PARTITION_SUBTYPE_DATA_COREDUMP: return "data/core";
    case ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS: return "data/nvskey";
    case ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM: return "data/efuse";
    case ESP_PARTITION_SUBTYPE_DATA_UNDEFINED:return "data";
    case ESP_PARTITION_SUBTYPE_DATA_LITTLEFS: return "data/lfs";
    default:                                  return "data/?";
    }
}

// Walk the image header + segment headers in flash to determine the on-flash
// size of the running app. Does not verify SHA — diagnostics only.
size_t running_app_image_size()
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (!p) return 0;

    esp_image_header_t hdr;
    if (esp_partition_read(p, 0, &hdr, sizeof(hdr)) != ESP_OK) return 0;
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) return 0;

    size_t off = sizeof(hdr);
    for (uint8_t i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t sh;
        if (esp_partition_read(p, off, &sh, sizeof(sh)) != ESP_OK) return 0;
        off += sizeof(sh) + sh.data_len;
        if (off > p->size) return 0;
    }
    // Pad to 16 bytes, add 1 checksum byte, optional 32-byte SHA256.
    off = (off + 15) & ~size_t{15};
    off += 1;
    if (hdr.hash_appended) off += 32;
    return off;
}

void format_kb(char *out, size_t n, size_t bytes)
{
    if (bytes >= 1024 * 1024) {
        snprintf(out, n, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(out, n, "%u KB", (unsigned)((bytes + 512) / 1024));
    } else {
        snprintf(out, n, "%u B", (unsigned)bytes);
    }
}

class MemInfoApp : public App {
public:
    const char *name() const override { return "Memory"; }
    const char *icon() const override { return "MEM"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_meminfo; }
    const char *slug() const override { return "meminfo"; }
    AppAccess access() const override { return AppAccess::Internal; }

    void on_enter(lv_obj_t *root) override
    {
        // ── Title ───────────────────────────────────────────────────────────
        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Memory & Storage");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        // ── Build static (partition) section ────────────────────────────────
        char *p = static_buf_;
        char *end = static_buf_ + sizeof(static_buf_);

        p += snprintf(p, end - p, "Flash partitions\n");

        uint32_t flash_total = 0;
        esp_flash_get_size(nullptr, &flash_total);

        size_t app_used = running_app_image_size();
        const esp_partition_t *running = esp_ota_get_running_partition();

        esp_partition_iterator_t it = esp_partition_find(
            ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
        while (it && p < end) {
            const esp_partition_t *pp = esp_partition_get(it);

            char size_str[40];
            if (running && pp->address == running->address && app_used) {
                char u[16], t[16];
                format_kb(u, sizeof(u), app_used);
                format_kb(t, sizeof(t), pp->size);
                snprintf(size_str, sizeof(size_str), "%s/%s", u, t);
            } else if (pp->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS) {
                size_t lfs_total = 0, lfs_used = 0;
                if (esp_littlefs_info(pp->label, &lfs_total, &lfs_used) == ESP_OK) {
                    char u[16], t[16];
                    format_kb(u, sizeof(u), lfs_used);
                    format_kb(t, sizeof(t), lfs_total);
                    snprintf(size_str, sizeof(size_str), "%s/%s", u, t);
                } else {
                    format_kb(size_str, sizeof(size_str), pp->size);
                }
            } else {
                format_kb(size_str, sizeof(size_str), pp->size);
            }

            p += snprintf(p, end - p, "  %-8s %-9s %s\n",
                pp->label,
                subtype_label(pp->type, pp->subtype),
                size_str);
            it = esp_partition_next(it);
        }
        if (it) esp_partition_iterator_release(it);

        char ftot[16];
        format_kb(ftot, sizeof(ftot), flash_total);
        p += snprintf(p, end - p, "  (chip: %s)\n", ftot);

        // ── Layout: static label on top, dynamic RAM label below ────────────
        static_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(static_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(static_label_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_pos(static_label_, 8, 26);
        lv_obj_set_width(static_label_, 304);
        lv_label_set_text(static_label_, static_buf_);

        ram_label_ = lv_label_create(root);
        lv_obj_set_style_text_font(ram_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(ram_label_, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align_to(ram_label_, static_label_, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);
        lv_obj_set_width(ram_label_, 304);

        render_ram();
        tick_ = 0;
    }

    void on_exit() override
    {
        static_label_ = nullptr;
        ram_label_ = nullptr;
    }

    void on_tick() override
    {
        if (++tick_ < 25) return; // ~500 ms (on_tick fires every 20 ms)
        tick_ = 0;
        render_ram();
    }

private:
    void render_ram()
    {
        if (!ram_label_) return;

        size_t dram_total = heap_caps_get_total_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t dram_free  = heap_caps_get_free_size(MALLOC_CAP_8BIT  | MALLOC_CAP_INTERNAL);
        size_t dram_min   = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        size_t dram_used  = (dram_total > dram_free) ? (dram_total - dram_free) : 0;

        // Largest contiguous block — useful when fragmentation matters.
        size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

        char tot[16], used[16], freeb[16], minb[16], largest[16];
        format_kb(tot,  sizeof(tot),  dram_total);
        format_kb(used, sizeof(used), dram_used);
        format_kb(freeb,sizeof(freeb),dram_free);
        format_kb(minb, sizeof(minb), dram_min);
        format_kb(largest, sizeof(largest), dram_largest);

        unsigned pct = dram_total ? (unsigned)((dram_used * 100) / dram_total) : 0;

        char buf[256];
        snprintf(buf, sizeof(buf),
            "Internal RAM (heap)\n"
            "  used  %s  (%u%%)\n"
            "  free  %s   min %s\n"
            "  largest block  %s\n"
            "  total %s",
            used, pct,
            freeb, minb,
            largest,
            tot);
        lv_label_set_text(ram_label_, buf);
    }

    lv_obj_t *static_label_ = nullptr;
    lv_obj_t *ram_label_ = nullptr;
    char static_buf_[512] = {};
    int tick_ = 0;
};

MemInfoApp s_instance;

} // namespace

App *app_meminfo_instance() { return &s_instance; }
