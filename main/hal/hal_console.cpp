#include "hal_console.h"
#include "admin_auth.h"
#include "hal_buttons.h"
#include "hal_config.h"
#include "hal_fs.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "hal_sleep.h"
#include "hal_radio.h"
#include "hal_screencap.h"
#include "hal_uitree.h"
#include "app.h"
#include "app_registry.h"
#include "launcher_slugs.h"
#include "app_launcher.h"
#include "blindbox_web_serial.h"
#include "bbx_xfer.h"
#include "app_chess.h"
#include "fs_snapshot.h"
#include "ripple_state.h"
#include "script_app.h"
#include "sponsor_layout.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "lvgl.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

static const char *TAG = "hal_console";
static const char *MOUNT_POINT = "/littlefs";


// Anchor relative paths under /littlefs; absolute paths are used verbatim.
static bool resolve_path(const char *in, char *out, size_t out_len)
{
    if (!in) return false;
    if (in[0] == '/') {
        size_t len = strlen(in);
        if (len >= out_len) return false;
        memcpy(out, in, len + 1);
    } else {
        int n = snprintf(out, out_len, "%s/%s", MOUNT_POINT, in);
        if (n < 0 || (size_t)n >= out_len) return false;
    }
    return true;
}

static int cmd_press(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: press <A|B|Home|Down|Left|Right|Up|Aux1|Start>\n");
        return 1;
    }
    Button b;
    if (!hal_buttons_button_from_name(argv[1], &b)) {
        printf("unknown button: %s\n", argv[1]);
        return 1;
    }
    hal_buttons_inject_press(b);
    return 0;
}

static int cmd_ls(int argc, char **argv)
{
    char path[160];
    const char *arg = (argc >= 2) ? argv[1] : "";
    if (!resolve_path(arg, path, sizeof(path))) { printf("path too long\n"); return 1; }
    DIR *d = opendir(path);
    if (!d) { printf("opendir %s: %s\n", path, strerror(errno)); return 1; }
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            const char *kind = S_ISDIR(st.st_mode) ? "d" : "-";
            printf("%s %8ld  %s\n", kind, (long)st.st_size, de->d_name);
        } else {
            printf("?        -  %s\n", de->d_name);
        }
    }
    closedir(d);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    if (argc != 2) { printf("usage: cat <path>\n"); return 1; }
    char path[160];
    if (!resolve_path(argv[1], path, sizeof(path))) { printf("path too long\n"); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("open %s: %s\n", path, strerror(errno)); return 1; }
    char buf[128];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, stdout);
    fclose(f);
    fputc('\n', stdout);
    return 0;
}

// Dump the whole filesystem as the JSON blob the Sync app uploads.
//
// This is also the badge->host download that backup, restore and the admin
// tooling need: `cat` is not binary-safe and there is no `get`. Piping this
// into a file gives a host-side copy of everything on the badge that is text.
//
// `snapshot stats` prints only the counters, which is what you want when
// checking how long the walk takes or why `complete` came back false.
static int cmd_snapshot(int argc, char **argv)
{
    bool stats_only = (argc >= 2 && strcmp(argv[1], "stats") == 0);

    size_t largest = 0, freed = 0;
    fs_snapshot_heap_info(&largest, &freed);
    size_t cap = fs_snapshot_pick_cap(kFsSnapshotCap);
    if (cap == 0) {
        printf("no room for a snapshot: largest block %u, free %u, need %u\n",
               (unsigned)largest, (unsigned)freed, (unsigned)kFsSnapshotMinCap);
        return 1;
    }
    if (cap < kFsSnapshotCap) {
        printf("# heap tight: cap %u not %u (largest %u, free %u)\n",
               (unsigned)cap, (unsigned)kFsSnapshotCap,
               (unsigned)largest, (unsigned)freed);
    }

    FsSnapshotStats st;
    char *json = fs_snapshot_build(MOUNT_POINT, cap, &st);
    if (!json) { printf("snapshot failed (out of memory)\n"); return 1; }

    if (!stats_only) {
        // Straight to stdout rather than through the log, so the host can
        // redirect it into a file and parse it.
        fwrite(json, 1, st.bytes, stdout);
        fputc('\n', stdout);
    }

    printf("bytes=%u crc32=%08lx complete=%s elapsed_ms=%u\n",
           (unsigned)st.bytes, (unsigned long)st.crc32,
           st.complete ? "yes" : "no", (unsigned)st.elapsed_ms);
    printf("seen=%u text=%u binary=%u too_big=%u dropped=%u unreadable=%u "
           "truncated=%u path_skipped=%u\n",
           (unsigned)st.files_seen, (unsigned)st.files_text,
           (unsigned)st.files_binary, (unsigned)st.files_too_big,
           (unsigned)st.files_dropped, (unsigned)st.files_unreadable,
           (unsigned)st.truncated, (unsigned)st.paths_skipped);

    fs_snapshot_free(json);
    return 0;
}

// Stream the live screen to the host so a script can see what the badge is
// showing. The badge has no framebuffer to copy — see hal_screencap.cpp for why
// this rides LVGL's flush path instead.
static int cmd_shot(int /*argc*/, char ** /*argv*/)
{
    ScreenCapStats st;
    if (!hal_screencap_stream(&st)) {
        printf("capture failed (display not up)\n");
        return 1;
    }
    // hal_screencap_stream() already printed the SHOT/END frame. Only a partial
    // capture needs comment, and it's a real failure for the host: a torn image
    // that decodes cleanly is worse than an error.
    if (!st.complete) {
        printf("# incomplete: covered %u of %u pixels\n",
               (unsigned)st.pixels_covered, (unsigned)st.w * st.h);
        return 1;
    }
    return 0;
}

// Text counterpart to `shot`: what the UI *is*, rather than what it looks
// like. Cheaper than a screenshot by three orders of magnitude, and it diffs.
static int cmd_uitree(int /*argc*/, char ** /*argv*/)
{
    UiTreeStats st;
    if (!hal_uitree_dump(&st)) {
        printf("uitree failed (display not up)\n");
        return 1;
    }
    return st.truncated ? 1 : 0;
}

// Last transfer's numbers. Venue RF is the thing this project understands
// least, so tuning the dwell and timeout constants should come from readings
// rather than from how a sync "felt".
static int cmd_xfer(int /*argc*/, char ** /*argv*/)
{
    static const char *const kStates[] = {
        "Idle", "Opening", "Backoff", "Sweeping", "AwaitStat",
        "Repairing", "AwaitDone", "Done", "Failed",
    };
    static const char *const kFails[] = {
        "None", "Radio", "NoStation", "StationBusy", "Lost",
        "Stalled", "Crc", "TooBig", "Server",
    };

    const XferStats *s = bbx_xfer_stats();
    unsigned st = (unsigned)bbx_xfer_state();
    unsigned fl = (unsigned)bbx_xfer_fail();

    printf("state=%s fail=%s server_status=%u progress=%d%%\n",
           st < (sizeof(kStates) / sizeof(kStates[0])) ? kStates[st] : "?",
           fl < (sizeof(kFails) / sizeof(kFails[0])) ? kFails[fl] : "?",
           bbx_xfer_server_status(), bbx_xfer_progress_pct());
    printf("frames=%u recv=%u repairs=%u sent=%lu elapsed_ms=%lu\n",
           s->total_frames, s->recv_count, s->repair_rounds,
           (unsigned long)s->frames_sent, (unsigned long)s->elapsed_ms);

    char id[BBX_MAX_BOX_ID + 1];
    int  rssi = 0;
    if (bbx_xfer_station_seen(id, sizeof(id), &rssi)) {
        printf("station=\"%s\" rssi=%d dBm\n", id, rssi);
    } else {
        printf("station=none heard recently\n");
    }
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    if (argc != 2) { printf("usage: rm <path>\n"); return 1; }
    char path[160];
    if (!resolve_path(argv[1], path, sizeof(path))) { printf("path too long\n"); return 1; }
    if (unlink(path) != 0) { printf("rm %s: %s\n", path, strerror(errno)); return 1; }
    return 0;
}

static int cmd_mkdir(int argc, char **argv)
{
    if (argc != 2) { printf("usage: mkdir <path>\n"); return 1; }
    char path[160];
    if (!resolve_path(argv[1], path, sizeof(path))) { printf("path too long\n"); return 1; }
    if (!hal_fs_mkdir_p(path)) { printf("mkdir failed\n"); return 1; }
    return 0;
}

// Read raw bytes from stdin into <path>. Host should send exactly <size>
// bytes after seeing the "READY" line. No line-ending translation is done
// for the payload — host is responsible for sending the file as-is.
static int cmd_put(int argc, char **argv)
{
    // Uploading means a host is driving this badge. Light sleep would suspend
    // USB-Serial-JTAG mid-transfer and the host would just see a timeout.
    hal_sleep_note_activity();
    if (argc == 2 && strcmp(argv[1], "--binary") == 0) {
        printf("PUT BINARY OK\n");
        return 0;
    }
    if (argc != 3) { printf("usage: put <path> <size>\n"); return 1; }
    char path[160];
    if (!resolve_path(argv[1], path, sizeof(path))) { printf("path too long\n"); return 1; }
    char *end = nullptr;
    long size = strtol(argv[2], &end, 10);
    if (!end || *end != 0 || size < 0) { printf("bad size\n"); return 1; }

    char parent[160];
    size_t plen = strlen(path);
    if (plen >= sizeof(parent)) { printf("path too long\n"); return 1; }
    memcpy(parent, path, plen + 1);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        hal_fs_mkdir_p(parent);
    }

    FILE *f = fopen(path, "wb");
    if (!f) { printf("open %s: %s\n", path, strerror(errno)); return 1; }

    // Console commands use CR line endings. Leaving that translation enabled
    // for the payload silently turns every binary 0x0d into 0x0a. Switch before
    // READY so the host cannot race us, and restore it on every return path.
    struct RawPayload {
        RawPayload() { usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF); }
        ~RawPayload() { usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR); }
    } raw_payload;

    printf("READY\n");
    fflush(stdout);

    long remaining = size;
    char buf[256];
    int fd = fileno(stdin);
    while (remaining > 0) {
        size_t want = (remaining < (long)sizeof(buf)) ? (size_t)remaining : sizeof(buf);
        int n = read(fd, buf, want);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        if (fwrite(buf, 1, n, f) != (size_t)n) {
            printf("write error\n");
            fclose(f);
            return 1;
        }
        remaining -= n;
    }
    fclose(f);
    if (remaining != 0) { printf("short read: %ld bytes missing\n", remaining); return 1; }
    printf("OK %ld\n", size);
    return 0;
}

// Legacy Web Serial Solana Scavenger Hunt response path. The app went
// proximity-only and no longer accepts these — the command line carries only
// request id + byte count, and the submit stub always rejects. Kept so tooling
// that still probes the command gets a clean BBX_WEB_REJECTED instead of
// "unknown command".
static int cmd_blindbox_response(int argc, char **argv)
{
    hal_sleep_note_activity();
    if (argc != 3) {
        printf("usage: blindbox_response <request_id> <size>\n");
        return 1;
    }

    char *id_end = nullptr;
    unsigned long request_id = strtoul(argv[1], &id_end, 10);
    char *size_end = nullptr;
    unsigned long size = strtoul(argv[2], &size_end, 10);
    if (!id_end || *id_end || request_id == 0 || request_id > UINT16_MAX ||
        !size_end || *size_end || size == 0 || size > BLINDBOX_WEB_MAX_RESPONSE) {
        printf("BBX_WEB_REJECTED invalid arguments\n");
        return 1;
    }

    char body[BLINDBOX_WEB_MAX_RESPONSE + 1];
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        printf("BBX_WEB_REJECTED %lu\n", request_id);
        return 1;
    }
    printf("BBX_WEB_READY %lu\n", request_id);
    fflush(stdout);

    size_t offset = 0;
    const TickType_t started = xTaskGetTickCount();
    while (offset < size && xTaskGetTickCount() - started < pdMS_TO_TICKS(10000)) {
        int n = read(fd, body + offset, size - offset);
        if (n > 0) offset += static_cast<size_t>(n);
        else if (n < 0 && errno != EINTR && errno != EAGAIN) break;
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    fcntl(fd, F_SETFL, flags);
    body[offset] = '\0';

    // App lifecycle fields belong to the LVGL task. Receive USB first, then
    // serialize validation/copying with app switches and timeout handling.
    bool accepted = false;
    if (offset == size && lvgl_port_lock(0)) {
        accepted = blindbox_web_serial_submit((uint16_t)request_id, body, offset);
        lvgl_port_unlock();
    }
    if (!accepted) {
        printf("BBX_WEB_REJECTED %lu\n", request_id);
        return 1;
    }
    printf("BBX_WEB_ACCEPTED %lu\n", request_id);
    return 0;
}

static int cmd_apps(int argc, char **argv)
{
    (void)argc; (void)argv;
    size_t n = app_registry_count();
    for (size_t i = 0; i < n; i++) {
        App *a = app_registry_get(i);
        if (!a) continue;
        printf("%-16s  %-3s  %s\n", a->slug(), a->icon(), a->name());
    }
    return 0;
}

// Re-scan /littlefs/apps for newly uploaded Lua apps without rebooting.
// Existing scripts refresh their launcher metadata as well as discovering
// newly installed apps, so adding/replacing a thumbnail needs no reboot.
static int cmd_reload(int argc, char **argv)
{
    (void)argc; (void)argv;
    // The registry vector is iterated on the LVGL task (launcher grid);
    // hold its lock across the rescan like shot/uitree do for reads.
    if (!lvgl_port_lock(0)) {
        printf("reload: LVGL is busy, try again\n");
        return 1;
    }
    size_t before = app_registry_count();
    script_app_register_all("/littlefs/apps");
    size_t after = app_registry_count();
    app_registry_return_to_launcher();
    lvgl_port_unlock();
    printf("reload: %u new app%s\n", (unsigned)(after - before),
           after - before == 1 ? "" : "s");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("rebooting...\n");
    fflush(stdout);
    esp_restart();
    return 0;
}

// Set the owner's bump card in one shot. Fields are pipe-delimited so values
// with spaces work without quoting: card <name>|<email>|<url>|<tag>
static int cmd_card(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: card <name>|<email>|<url>|<tag>\n");
        return 1;
    }

    char joined[256];
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        size_t need = strlen(argv[i]) + (i + 1 < argc ? 1 : 0);
        if (off + need >= sizeof(joined)) { printf("input too long\n"); return 1; }
        memcpy(joined + off, argv[i], strlen(argv[i]));
        off += strlen(argv[i]);
        if (i + 1 < argc) joined[off++] = ' ';
    }
    joined[off] = '\0';

    const char *fields[4] = { nullptr, nullptr, nullptr, nullptr };
    int n = 0;
    char *p = joined;
    fields[n++] = p;
    while (*p && n < 4) {
        if (*p == '|') {
            *p = '\0';
            fields[n++] = p + 1;
        }
        p++;
    }
    while (*p) {
        if (*p == '|') { printf("too many fields (expected 4)\n"); return 1; }
        p++;
    }
    if (n != 4) { printf("expected 4 fields separated by '|', got %d\n", n); return 1; }

    static const struct { const char *key; size_t max; } spec[] = {
        { "name",  32 },
        { "email", 48 },
        { "url",   64 },
        { "tag",   24 },
    };
    for (int i = 0; i < 4; i++) {
        if (strlen(fields[i]) >= spec[i].max) {
            printf("%s too long (max %u)\n", spec[i].key, (unsigned)(spec[i].max - 1));
            return 1;
        }
    }

    for (int i = 0; i < 4; i++) {
        hal_config_set_string("card", spec[i].key, fields[i]);
    }
    printf("OK name=%s email=%s url=%s tag=%s\n",
           fields[0], fields[1], fields[2], fields[3]);
    return 0;
}

// Generic config setter: writes /littlefs/config/<slug>.cfg via hal_config so
// it composes with apps that already read those slugs (e.g. the card app's
// name/email/url/tag). For values with spaces, quote: config card name "Ada L"
static int cmd_config(int argc, char **argv)
{
    if (argc != 4) {
        printf("usage: config <slug> <key> <value>\n");
        return 1;
    }
    hal_config_set_string(argv[1], argv[2], argv[3]);
    printf("OK %s.%s=%s\n", argv[1], argv[2], argv[3]);
    return 0;
}

static void print_ripple_command()
{
    const RippleState state = ripple_state_load();
    if (state.active) {
        printf("ripple set %u\n", static_cast<unsigned>(state.depth));
    } else {
        printf("ripple clear\n");
    }
}

static int cmd_ripple(int argc, char **argv)
{
    if (argc == 1) {
        print_ripple_command();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "seed") == 0) {
        if (!ripple_state_seed()) {
            printf("could not seed Ripple\n");
            return 1;
        }
        print_ripple_command();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "clear") == 0) {
        if (!ripple_state_clear()) {
            printf("could not clear Ripple\n");
            return 1;
        }
        print_ripple_command();
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "set") == 0) {
        char *end = nullptr;
        const unsigned long depth = strtoul(argv[2], &end, 10);
        if (!end || end == argv[2] || *end != '\0' || depth > kRippleMaxDepth
            || !ripple_state_set_depth(static_cast<unsigned>(depth))) {
            printf("usage: ripple [seed|clear|set <0..%u>]\n",
                   static_cast<unsigned>(kRippleMaxDepth));
            return 1;
        }
        print_ripple_command();
        return 0;
    }

    printf("usage: ripple [seed|clear|set <0..%u>]\n",
           static_cast<unsigned>(kRippleMaxDepth));
    return 1;
}

static void print_sponsor_map_command()
{
    SponsorLayout layout;
    sponsor_layout_load(&layout);
    printf("sponsormap");
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        printf(" %u", static_cast<unsigned>(layout.region_counts[i]));
    }
    printf("\n");
}

// Configure the six physical booth groups in one copy/pasteable command. A
// no-argument call intentionally prints the command that recreates the active
// layout, which makes day-of auditing and cloning badges straightforward.
static int cmd_sponsormap(int argc, char **argv)
{
    if (argc == 1) {
        print_sponsor_map_command();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        if (!sponsor_layout_save_defaults()) {
            printf("could not save default sponsor map\n");
            return 1;
        }
        printf("OK default sponsor map restored; reopen Sponsors to refresh\n");
        print_sponsor_map_command();
        return 0;
    }
    if (argc != static_cast<int>(kSponsorRegionCount + 1)) {
        printf("usage: sponsormap <A> <B> <C> <D> <E> <F> | reset\n");
        return 1;
    }

    unsigned counts[kSponsorRegionCount] = {};
    for (size_t i = 0; i < kSponsorRegionCount; ++i) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(argv[i + 1], &end, 10);
        if (!end || end == argv[i + 1] || *end != '\0'
            || parsed > kSponsorRegionMaxTables) {
            printf("bad region %s count: use 0..%u\n",
                   sponsor_layout_region_name(i), kSponsorRegionMaxTables);
            return 1;
        }
        counts[i] = static_cast<unsigned>(parsed);
    }
    if (!sponsor_layout_save_counts(counts, kSponsorRegionCount)) {
        printf("invalid layout: total must be 1..%u tables\n",
               static_cast<unsigned>(kSponsorMapMaxTables));
        return 1;
    }

    printf("OK sponsor map saved; reopen Sponsors to refresh\n");
    print_sponsor_map_command();
    return 0;
}

static int cmd_logo(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char logo[] = R"(
 __    __   ______    ______   __    __        ________  __    __  ________        __    __   ______   _______  ________  __    __
|  \  |  \ /      \  /      \ |  \  /  \      |        \|  \  |  \|        \      |  \  |  \ /      \ |       \|        \|  \  |  \
| $$  | $$|  $$$$$$\|  $$$$$$\| $$ /  $$       \$$$$$$$$| $$  | $$| $$$$$$$$      | $$\ | $$|  $$$$$$\| $$$$$$$\\$$$$$$$$| $$  | $$
| $$__| $$| $$__| $$| $$   \$$| $$/  $$          | $$   | $$__| $$| $$__          | $$$\| $$| $$  | $$| $$__| $$  | $$   | $$__| $$
| $$    $$| $$    $$| $$      | $$  $$           | $$   | $$    $$| $$  \         | $$$$\ $$| $$  | $$| $$    $$  | $$   | $$    $$
| $$$$$$$$| $$$$$$$$| $$   __ | $$$$$\           | $$   | $$$$$$$$| $$$$$         | $$\$$ $$| $$  | $$| $$$$$$$\  | $$   | $$$$$$$$
| $$  | $$| $$  | $$| $$__/  \| $$ \$$\          | $$   | $$  | $$| $$_____       | $$ \$$$$| $$__/ $$| $$  | $$  | $$   | $$  | $$
| $$  | $$| $$  | $$ \$$    $$| $$  \$$\         | $$   | $$  | $$| $$     \      | $$  \$$$ \$$    $$| $$  | $$  | $$   | $$  | $$
 \$$   \$$ \$$   \$$  \$$$$$$  \$$   \$$          \$$    \$$   \$$ \$$$$$$$$       \$$   \$$  \$$$$$$  \$$   \$$   \$$    \$$   \$$
)";
    fputs(logo, stdout);
    return 0;
}

static int cmd_debug(int argc, char **argv)
{
    if (argc != 2 || !admin_password_valid(argv[1])) {
        printf("ADMIN AUTH FAIL\nusage: debug <admin-password>\n");
        return 1;
    }
    lvgl_port_lock(0);
    size_t added = launcher_slugs_unlock_internal_apps();
    lvgl_port_unlock();
    printf("diagnostic apps: %zu added\n", added);
    return 0;
}

// Every build supports the same provisioning protocol. Reading the mode is
// public; checking credentials or changing the mode requires Admin password.
static int cmd_appmode(int argc, char **argv)
{
    if (argc == 1) {
        char mode[16];
        hal_config_get_string("system", "app_mode", mode, sizeof(mode),
                              "default");
        printf("APP MODE %s\n", mode);
        return 0;
    }
    if (argc != 3 || !admin_password_valid(argv[2])) {
        printf("ADMIN AUTH FAIL\nusage: appmode <default|public|all|check> <admin-password>\n");
        return 1;
    }

    // Preflight: host tools validate access before writing an identity.
    if (strcmp(argv[1], "check") == 0) {
        printf("ADMIN AUTH OK\n");
        return 0;
    }
    LauncherAppMode mode;
    const char *name;
    if (strcmp(argv[1], "default") == 0) {
        mode = LauncherAppMode::DefaultsOnly;
        name = "default";
    } else if (strcmp(argv[1], "public") == 0) {
        mode = LauncherAppMode::PublicApps;
        name = "public";
    } else if (strcmp(argv[1], "all") == 0) {
        mode = LauncherAppMode::PublicAndInternal;
        name = "all";
    } else {
        printf("unknown app mode\nusage: appmode <default|public|all> <admin-password>\n");
        return 1;
    }

    lvgl_port_lock(0);
    launcher_slugs_set_mode(mode);
    lvgl_port_unlock();
    printf("APP MODE OK %s\n", name);
    return 0;
}

static bool rm_rf(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;

    DIR *d = opendir(path);
    if (!d) return false;
    bool ok = true;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[256];
        int n = snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) { ok = false; continue; }
        if (!rm_rf(child)) ok = false;
    }
    closedir(d);
    if (rmdir(path) != 0) ok = false;
    return ok;
}

// NOTE: this must NEVER remove /littlefs/identity.json or
// /littlefs/solana.json. Both live at the LittleFS root precisely so this
// rm_rf can't reach them — a badge that loses its identity has to go back to
// the registration desk, and a badge that loses its Solana key loses the
// attendee's wallet. Do not "make this more thorough" by wiping /littlefs.
static int cmd_factory_reset(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("WARNING: wipes all config, contacts, and installed Lua apps.\n");
        printf("Identity and Solana keys are preserved.\n");
        printf("usage: factory_reset confirm\n");
        return 1;
    }
    bool ok = true;
    if (!rm_rf("/littlefs/config"))  ok = false;
    if (!rm_rf("/littlefs/apps"))    ok = false;
    if (!rm_rf("/littlefs/appdata")) ok = false;
    printf("factory_reset: %s. rebooting...\n", ok ? "cleared" : "partial (some entries failed)");
    fflush(stdout);
    esp_restart();
    return 0;
}

// Admin shortcut: pre-install every public app, no NFC scan needed. Internal
// tools remain gated behind the separate debug command.
static int cmd_seedall(int argc, char **argv)
{
    if (argc != 2 || !admin_password_valid(argv[1])) {
        printf("ADMIN AUTH FAIL\nusage: seedall <admin-password>\n");
        return 1;
    }
    lvgl_port_lock(0);
    size_t added = launcher_slugs_unlock_public_apps();
    lvgl_port_unlock();
    printf("public apps: %zu added\n", added);
    return 0;
}

// ---------------------------------------------------------------- provisioning

// The BLE static random address is only populated once NimBLE has synced, so
// bring the radio up if it isn't already. Registration MUST record this
// mac -> badge_id mapping: it is the only way to re-identify a badge that has
// lost its identity file, without the attendee remembering anything.
//
// We deliberately leave the radio enabled afterwards. Disabling blocks up to
// 2 s tearing down the NimBLE host, which is dead time on every badge in a
// 1000-unit queue; the app registry force-disables on the next app switch
// anyway, and an idle badge only advertises a PII-free placeholder.
static bool console_mac(uint8_t out[6])
{
    hal_radio_local_mac(out);
    for (int i = 0; i < 6; i++) if (out[i]) return true;

    if (hal_radio_enable() != ESP_OK) return false;
    hal_radio_local_mac(out);
    for (int i = 0; i < 6; i++) if (out[i]) return true;
    return false;
}

static void print_mac(void)
{
    uint8_t mac[6] = {};
    if (!console_mac(mac)) {
        printf("mac=unavailable\n");
        return;
    }
    printf("mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Glanceable receipt for an operator holding a stack of badges: three green
// flashes then the role colour on success, solid red on failure. This runs on
// the console task while an app may also be driving the LEDs from the LVGL
// tick; during registration the foreground app is the ID page, which doesn't
// animate, so they don't fight in practice.
static void prov_leds(bool ok, const uint8_t rgb[3])
{
    if (!ok) {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 64, 0, 0);
        hal_lights_show();
        return;
    }
    for (int flash = 0; flash < 3; flash++) {
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, 0, 80, 0);
        hal_lights_show();
        vTaskDelay(pdMS_TO_TICKS(120));
        hal_lights_clear();
        hal_lights_show();
        vTaskDelay(pdMS_TO_TICKS(90));
    }
    for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, rgb[0], rgb[1], rgb[2]);
    hal_lights_show();
}

// Credential bytes never enter REPL history, identity.json or snapshot output.
// Reading is an explicit USB-only operation for trusted host sync tools.
static int cmd_badge_token(int argc, char **argv)
{
    hal_sleep_note_activity();
    char token[65] = {};
    if (argc == 2 && strcmp(argv[1], "get") == 0) {
        if (!hal_identity_get_upload_token(token)) {
            printf("BADGE_TOKEN MISSING\n");
            return 1;
        }
        printf("BADGE_TOKEN %s\n", token);
        memset(token, 0, sizeof(token));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "set") == 0) {
        int fd = fileno(stdin);
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            printf("BADGE_TOKEN FAIL\n");
            return 1;
        }
        printf("BADGE_TOKEN READY\n");
        fflush(stdout);
        size_t got = 0;
        TickType_t started = xTaskGetTickCount();
        while (got < 64 && xTaskGetTickCount() - started < pdMS_TO_TICKS(10000)) {
            int n = read(fd, token + got, 64 - got);
            if (n > 0) got += n;
            else vTaskDelay(pdMS_TO_TICKS(10));
        }
        fcntl(fd, F_SETFL, flags);
        bool ok = got == 64 && hal_identity_set_upload_token(token) == ESP_OK;
        memset(token, 0, sizeof(token));
        printf("BADGE_TOKEN %s\n", ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }
    printf("usage: badge_token <set|get>\n");
    return 1;
}

static int cmd_badge_profile(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "apply") != 0) {
        printf("usage: badge_profile apply <bytes>\n");
        return 1;
    }
    char *end = nullptr;
    unsigned long len = strtoul(argv[2], &end, 10);
    if (!end || *end || len == 0 || len > 2048) { printf("BADGE_PROFILE FAIL\n"); return 1; }
    char *json = (char *)calloc(len + 1, 1);
    if (!json) { printf("BADGE_PROFILE FAIL\n"); return 1; }
    int fd = fileno(stdin);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        free(json); printf("BADGE_PROFILE FAIL\n"); return 1;
    }
    hal_sleep_note_activity();
    printf("BADGE_PROFILE READY\n");
    fflush(stdout);
    size_t got = 0;
    TickType_t started = xTaskGetTickCount();
    while (got < len && xTaskGetTickCount() - started < pdMS_TO_TICKS(10000)) {
        int n = read(fd, json + got, len - got);
        if (n > 0) got += n;
        else vTaskDelay(pdMS_TO_TICKS(10));
    }
    fcntl(fd, F_SETFL, flags);
    bool ok = false;
    // The open badge card reads identity on the LVGL task. Publish the new
    // record while holding its lock, after USB reception has finished.
    if (got == len && lvgl_port_lock(0)) {
        ok = hal_identity_apply_profile(json, len) == ESP_OK;
        lvgl_port_unlock();
    }
    memset(json, 0, len);
    free(json);
    printf("BADGE_PROFILE %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}

static int cmd_prov(int argc, char **argv)
{
    hal_sleep_note_activity();
    const char *sub = (argc >= 2) ? argv[1] : "";

    if (strcmp(sub, "apply") == 0) {
        esp_err_t err = ESP_FAIL;
        if (lvgl_port_lock(0)) {
            err = hal_identity_reload();
            lvgl_port_unlock();
        }
        if (err != ESP_OK || !hal_identity_is_provisioned()) {
            // The JSON was written straight to its final path, so a bad record
            // leaves the badge unprovisioned rather than half-loaded. It boots
            // to the "see registration" screen; just re-do the badge.
            printf("PROV FAIL invalid or missing %s\n", BADGE_IDENTITY_PATH);
            prov_leds(false, nullptr);
            return 1;
        }
        // A badge that just got an owner is about to be handed to someone who
        // has never used one. Replay the tutorial regardless of whether this
        // unit already ran it during testing or a previous registration.
        badge_onboarding_reset();

        const BadgeIdentity *id = hal_identity_get();
        printf("PROV OK id=%s ", id->badge_id);
        print_mac();
        printf("name=%s role=%s attendee=%lu tutorial=reset\n",
               id->display_name, hal_identity_role_name(id->role),
               (unsigned long)id->attendee_id);
        prov_leds(true, id->role_color);
        return 0;
    }

    if (strcmp(sub, "show") == 0) {
        const BadgeIdentity *id = hal_identity_get();
        printf("provisioned=%d\n", id->provisioned ? 1 : 0);
        if (id->provisioned) {
            printf("badge_id=%s\n",     id->badge_id);
            printf("attendee_id=%lu\n", (unsigned long)id->attendee_id);
            printf("display_name=%s\n", id->display_name);
            printf("account_email=%s\n", id->account_email);
            printf("claim_id=%s\n",     id->claim_id);
            printf("role=%s\n",         hal_identity_role_name(id->role));
            printf("role_color=#%02x%02x%02x\n",
                   id->role_color[0], id->role_color[1], id->role_color[2]);
            printf("net_email=%s\n",     id->net_email);
            printf("net_phone=%s\n",     id->net_phone);
            printf("net_linkedin=%s\n",  id->net_linkedin);
            printf("net_discord=%s\n",   id->net_discord);
            printf("net_instagram=%s\n", id->net_instagram);
            printf("net_x=%s\n",         id->net_x);
        }
        print_mac();
        return 0;
    }

    if (strcmp(sub, "mac") == 0) {
        print_mac();
        return 0;
    }

    if (strcmp(sub, "erase") == 0) {
        if (argc != 3 || strcmp(argv[2], "confirm") != 0) {
            printf("WARNING: anonymises this badge. Staff only.\n");
            printf("usage: prov erase confirm\n");
            return 1;
        }
        if (!lvgl_port_lock(0)) { printf("PROV FAIL display busy\n"); return 1; }
        hal_identity_erase();
        lvgl_port_unlock();
        printf("PROV ERASED\n");
        return 0;
    }

    printf("usage: prov <apply|show|mac|erase confirm>\n");
    printf("  provision flow: put %s <size> ... then: prov apply\n",
           BADGE_IDENTITY_PATH);
    return 1;
}

// Exposes the chess app's internals the way `xfer` exposes the transfer
// module's: enough state to drive and assert on a game from tools/badge
// without a human watching the screen, and without the three-orders-of-
// magnitude cost of a screenshot.
static int cmd_chess(int argc, char **argv)
{
    if (argc == 1) {
        char buf[512];
        if (!chess_app_debug(buf, sizeof(buf))) {
            printf("chess: not the active app (launch it first)\n");
            return 1;
        }
        fputs(buf, stdout);
        return 0;
    }

    if (strcmp(argv[1], "new") == 0) {
        if (!chess_app_new_game()) { printf("chess: not the active app\n"); return 1; }
        printf("OK new game\n");
        return 0;
    }

    if (strcmp(argv[1], "bump") == 0) {
        if (!chess_app_bump()) { printf("chess: not the active app\n"); return 1; }
        printf("OK bumped\n");
        return 0;
    }

    if (strcmp(argv[1], "flip") == 0) {
        if (!chess_app_flip()) { printf("chess: not the active app\n"); return 1; }
        printf("OK flipped\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "move") == 0) {
        if (!chess_app_play_uci(argv[2])) {
            printf("chess: illegal or unparseable move '%s'\n", argv[2]);
            return 1;
        }
        printf("OK %s\n", argv[2]);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "fen") == 0) {
        // A FEN has spaces in it. Rejoin the tail so it works whether or not
        // the caller quoted it — badge.py does, a human at the prompt may not.
        char fen[100];
        size_t n = 0;
        for (int i = 2; i < argc; i++) {
            const char *w = argv[i];
            if (i > 2 && n + 1 < sizeof(fen)) fen[n++] = ' ';
            while (*w && n + 1 < sizeof(fen)) fen[n++] = *w++;
        }
        fen[n] = '\0';
        if (!chess_app_set_fen(fen)) {
            printf("chess: rejected FEN '%s'\n", fen);
            return 1;
        }
        printf("OK %s\n", fen);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "perft") == 0) {
        int depth = atoi(argv[2]);
        // Capped at 4. Depth 5 from the start position is 4.8M nodes, which at
        // 80 MHz is minutes with the wake lock held — and perft is a rules
        // check, which the host harness in tools/chess-perft already does to
        // depth 5 in 150 ms.
        if (depth < 1) depth = 1;
        if (depth > 4) { printf("chess: perft depth capped at 4\n"); depth = 4; }
        uint64_t nodes = 0;
        uint32_t ms = 0;
        if (!chess_app_perft(depth, &nodes, &ms)) {
            printf("chess: not the active app, or out of memory\n");
            return 1;
        }
        printf("perft %d = %llu  (%lu ms)\n", depth,
               (unsigned long long)nodes, (unsigned long)ms);
        return 0;
    }

    printf("usage: chess [new|bump|flip|move <uci>|fen <FEN>|perft <1-4>]\n");
    return 1;
}

// System heap plus the LVGL pool side by side: the pool is carved out of the
// system heap once at boot, so a shrinking system number with a flat LVGL
// number means native allocations, and the reverse means widgets.
static int cmd_heap(int /*argc*/, char ** /*argv*/)
{
    lv_mem_monitor_t mon = {};
    // The pool mutates under the LVGL timer while we walk it.
    if (lvgl_port_lock(0)) {
        lv_mem_monitor(&mon);
        lvgl_port_unlock();
    }
    printf("sys_free=%u sys_largest=%u sys_min=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
    printf("lv_total=%u lv_free=%u lv_max_used=%u lv_used_pct=%u lv_frag_pct=%u\n",
           (unsigned)mon.total_size, (unsigned)mon.free_size,
           (unsigned)mon.max_used, mon.used_pct, mon.frag_pct);
    // Per-task stack margins (high-water free, bytes). Sticky lows: the
    // smallest number seen here across heavy use is the trim bound.
    UBaseType_t n = uxTaskGetNumberOfTasks() + 4;
    TaskStatus_t *tasks = (TaskStatus_t *)malloc(n * sizeof(TaskStatus_t));
    if (tasks) {
        uint32_t total_run = 0;
        n = uxTaskGetSystemState(tasks, n, &total_run);
        for (UBaseType_t i = 0; i < n; i++) {
            printf("task=%-12s prio=%2u stack_free=%u\n", tasks[i].pcTaskName,
                   (unsigned)tasks[i].uxCurrentPriority,
                   (unsigned)(tasks[i].usStackHighWaterMark * sizeof(StackType_t)));
        }
        free(tasks);
    }
    return 0;
}

static int cmd_radio(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "probe") == 0) {
        const esp_err_t result = hal_radio_probe();
        printf("probe_result=%s\n", esp_err_to_name(result));
    } else if (argc != 1) {
        printf("usage: radio [probe]\n");
        return 1;
    }
    char status[384];
    hal_radio_debug(status, sizeof(status));
    fputs(status, stdout);
    return 0;
}

void hal_console_start()
{
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "badge> ";
    repl_config.max_cmdline_length = 256;

    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));

    esp_console_register_help_command();

    const struct {
        const char *name;
        const char *help;
        esp_console_cmd_func_t func;
    } commands[] = {
        { "press",  "Inject a button press: press <name>",                   &cmd_press },
        { "shot",   "Stream the screen as RLE+base64 RGB565",                &cmd_shot },
        { "uitree", "Dump the LVGL widget tree as text",                     &cmd_uitree },
        { "ls",     "List directory: ls [path] (default /littlefs)",         &cmd_ls },
        { "cat",    "Print file contents: cat <path>",                       &cmd_cat },
        { "snapshot", "Dump the filesystem as JSON: snapshot [stats]",        &cmd_snapshot },
        { "xfer",   "Show the last sync transfer's stats",                    &cmd_xfer },
        { "radio",  "Radio diagnostics: radio [probe]; probe starts no advertisements or scan", &cmd_radio },
        { "heap",   "Heap usage: system heap plus LVGL pool",                &cmd_heap },
        { "chess",  "Chess: chess [new|flip|move <uci>|fen <FEN>|perft <n>]", &cmd_chess },
        { "rm",     "Delete file: rm <path>",                                &cmd_rm },
        { "mkdir",  "Create directory (with parents): mkdir <path>",         &cmd_mkdir },
        { "put",    "Upload file: put <path> <size> then send <size> bytes", &cmd_put },
        { "blindbox_response", "Legacy (always rejects): blindbox_response <id> <size>", &cmd_blindbox_response },
        { "apps",   "List installed apps",                                   &cmd_apps },
        { "reload", "Rescan /littlefs/apps for new Lua apps (no reboot)",      &cmd_reload },
        { "card",   "Set bump card: card <name>|<email>|<url>|<tag>",        &cmd_card },
        { "config", "Set a config value: config <slug> <key> <value>",       &cmd_config },
        { "ripple", "Get/set Ripple state: ripple [seed|clear|set <depth>]", &cmd_ripple },
        { "sponsormap", "Get/set booth groups: sponsormap [A B C D E F|reset]", &cmd_sponsormap },
        { "reboot", "Restart the badge",                                     &cmd_reboot },
        { "logo",   "Print the Hack the North ASCII logo",                   &cmd_logo },
        { "debug",  "Unlock diagnostics: debug <admin-password>",                      &cmd_debug },
        { "appmode", "Initial apps: appmode [default|public|all|check <admin-password>]",   &cmd_appmode },
        { "badge_token", "USB upload credential: badge_token <set|get>", &cmd_badge_token },
        { "badge_profile", "Apply server profile: badge_profile apply <bytes>", &cmd_badge_profile },
        { "prov",   "Provisioning: prov <apply|show|mac|erase confirm>",     &cmd_prov },
        { "seedall", "Unlock public apps: seedall <admin-password>",               &cmd_seedall },
        { "factory_reset", "Wipe config + Lua apps and reboot: factory_reset confirm", &cmd_factory_reset },
    };
    for (const auto &c : commands) {
        esp_console_cmd_t command = {};
        command.command = c.name;
        command.help = c.help;
        command.func = c.func;
        ESP_ERROR_CHECK(esp_console_cmd_register(&command));
    }

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "console started");
}
