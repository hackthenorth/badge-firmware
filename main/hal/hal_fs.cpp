#include "hal_fs.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "hal_fs";
static const char *MOUNT_POINT = "/littlefs";
static const char *PARTITION_LABEL = "storage";

esp_err_t hal_fs_init()
{
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = MOUNT_POINT;
    conf.partition_label = PARTITION_LABEL;
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_littlefs_register: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted at %s (used %u / %u bytes)",
                 MOUNT_POINT, (unsigned)used, (unsigned)total);
    } else {
        ESP_LOGI(TAG, "littlefs mounted at %s", MOUNT_POINT);
    }
    return ESP_OK;
}

bool hal_fs_mkdir_p(const char *path)
{
    if (!path || !*path) return false;

    char buf[128];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return false;
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "mkdir %s: %s", buf, strerror(errno));
            return false;
        }
        buf[i] = '/';
    }
    if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir %s: %s", buf, strerror(errno));
        return false;
    }
    return true;
}

bool hal_fs_usage(size_t *used_out, size_t *total_out)
{
    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) != ESP_OK) return false;
    if (used_out)  *used_out  = used;
    if (total_out) *total_out = total;
    return true;
}
