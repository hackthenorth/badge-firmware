#include "app_reboot.h"

#include "app.h"

#include "esp_log.h"
#include "esp_system.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>

static const char *TAG = "app_reboot";
static const char *kIntentPath = "/littlefs/config/reboot.cfg";
static constexpr size_t kSlugCap = sizeof(AppRebootPending::target);

// Stash for the launcher: consume_pending() deletes the file, but the grid
// needs the focus slug later when it enters. Same-boot RAM is enough — a
// power loss before the launcher enters falls back to the default cursor.
static char s_stashed_focus[kSlugCap] = {};

// Bounded slug copy. snprintf("%s") into a fixed buffer trips
// -Werror=format-truncation on unbounded input, so this spells it out;
// a slug that would not fit is refused rather than truncated (a truncated
// slug would boot somewhere unexpected).
static bool copy_slug(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0 || !src) return false;
    const size_t n = strlen(src);
    if (n == 0 || n >= dst_sz) return false;
    memcpy(dst, src, n + 1);
    return true;
}

// Fixed-format intent, written with stack buffers and raw POSIX I/O only:
// the OOM request path may run with barely any heap left, so this must not
// malloc (hal_config does) and must not depend on NVS.
static bool persist_intent(const char *target, const char *focus)
{
    if (!target) target = "";
    if (!focus) focus = "";
    const size_t tl = strlen(target);
    const size_t fl = strlen(focus);
    if (tl >= kSlugCap || fl >= kSlugCap) return false;
    if ((target[0] == '\0') == (focus[0] == '\0')) return false; // exactly one set

    char buf[160];
    size_t pos = 0;
    auto put = [&](const char *s, size_t n) {
        if (pos + n > sizeof(buf)) return false;
        memcpy(buf + pos, s, n);
        pos += n;
        return true;
    };
    if (!put("target=", 7) || !put(target, tl) || !put("\n", 1)) return false;
    if (!put("focus=", 6) || !put(focus, fl) || !put("\n", 1)) return false;

    const int fd = open(kIntentPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed", kIntentPath);
        return false;
    }
    size_t off = 0;
    while (off < pos) {
        const ssize_t w = write(fd, buf + off, pos - off);
        if (w <= 0) {
            close(fd);
            unlink(kIntentPath);
            return false;
        }
        off += (size_t)w;
    }
    close(fd);
    return true;
}

bool app_reboot_into_slug(const char *slug)
{
    if (!slug || !*slug) return false;
    ESP_LOGI(TAG, "reboot into %s", slug);
    if (!persist_intent(slug, "")) {
        ESP_LOGW(TAG, "intent persist failed; staying put");
        return false;
    }
    esp_restart();
    return true; // unreachable; keeps callers honest about the fallback
}

bool app_reboot_into_app(App *app)
{
    if (!app || !app->slug()) return false;
    return app_reboot_into_slug(app->slug());
}

bool app_reboot_to_launcher(const char *focus_slug)
{
    ESP_LOGI(TAG, "reboot to launcher focus=%s", focus_slug ? focus_slug : "");
    if (!focus_slug || !*focus_slug || !persist_intent("", focus_slug)) {
        ESP_LOGW(TAG, "intent persist failed; staying put");
        return false;
    }
    esp_restart();
    return true; // unreachable
}

static void clear_stash()
{
    s_stashed_focus[0] = '\0';
}

bool app_reboot_consume_pending(AppRebootPending *out)
{
    if (out) {
        out->target[0] = '\0';
        out->focus[0] = '\0';
        out->has_target = out->has_focus = false;
    }
    clear_stash();

    // Only our own esp_restart() may redirect the boot. Anything else —
    // brownout, power-on, panic, watchdog — clears the intent and boots
    // normally, so a dying boot can never loop back into itself.
    if (esp_reset_reason() != ESP_RST_SW) {
        unlink(kIntentPath);
        return false;
    }

    const int fd = open(kIntentPath, O_RDONLY);
    if (fd < 0) return false;
    char buf[256];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        const ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (r < 0) {
            close(fd);
            unlink(kIntentPath);
            return false;
        }
        if (r == 0) break;
        total += r;
    }
    close(fd);
    // Consume-first: even our own next restart boots normally, so one request
    // fires exactly once no matter what happens after this.
    unlink(kIntentPath);
    buf[total] = '\0';

    if (!out) return true;
    for (char *line = buf; line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        const char *value = nullptr;
        char *dst = nullptr;
        bool *has = nullptr;
        if (strncmp(line, "target=", 7) == 0) {
            value = line + 7;
            dst = out->target;
            has = &out->has_target;
        } else if (strncmp(line, "focus=", 6) == 0) {
            value = line + 6;
            dst = out->focus;
            has = &out->has_focus;
        }
        if (value && *value && copy_slug(dst, kSlugCap, value)) *has = true;
        line = nl ? nl + 1 : nullptr;
    }
    if (out->has_focus) copy_slug(s_stashed_focus, sizeof(s_stashed_focus), out->focus);
    return out->has_target || out->has_focus;
}

bool app_reboot_take_launcher_focus(char *slug_out, size_t slug_sz)
{
    if (!slug_out || slug_sz == 0) return false;
    slug_out[0] = '\0';
    if (!s_stashed_focus[0]) return false;
    if (!copy_slug(slug_out, slug_sz, s_stashed_focus)) return false;
    clear_stash();
    return true;
}
