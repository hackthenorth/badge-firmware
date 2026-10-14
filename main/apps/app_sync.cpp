#include "app.h"
#include "app_launcher.h"
#include "app_registry.h"
#include "bbx_xfer.h"
#include "fs_snapshot.h"
#include "hal_identity.h"
#include "hal_radio.h"
#include "hal_sleep.h"
#include "ui_theme.h"

#include "esp_log.h"
#include "esp_rom_crc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>   // unlink

namespace {

const char *const kSlug = "sync";

// Palette matches app_blindbox.cpp and app_launcher.cpp.
constexpr uint32_t COLOR_BG      = BadgeTheme::BACKGROUND;
constexpr uint32_t COLOR_PANEL   = BadgeTheme::PANEL;
constexpr uint32_t COLOR_BORDER  = BadgeTheme::BORDER;
constexpr uint32_t COLOR_ACCENT  = BadgeTheme::ACCENT_DETAIL;
constexpr uint32_t COLOR_TEXT    = BadgeTheme::TEXT;
constexpr uint32_t COLOR_DIM     = BadgeTheme::TEXT_MUTED;
constexpr uint32_t COLOR_GOLD    = 0xffd166;
constexpr uint32_t COLOR_MAGENTA = 0xff5c8a;
constexpr uint32_t COLOR_GREEN   = 0x36e07a;

// Registry tick timer runs at 50 Hz, so 1 tick = 20 ms.
constexpr int PULSE_PERIOD = 25;

// How long the "Synced" screen stays before the app is willing to sync again.
constexpr int kSettleTicks = 50;

// How long an error screen sticks before the app returns to the idle view.
// At 50 Hz a 400-tick hold is ~8 s: long enough to read while holding the
// badge still, short enough that a retry is never far away. Previously errors
// raised before bbx_xfer_start() (e.g. "Not enough memory") were overwritten
// by render_idle() on the very next tick, so they only blipped for 20 ms.
constexpr int kErrorHoldTicks = 400;

}  // namespace

extern "C" const lv_image_dsc_t icon_sync;

class SyncApp : public App {
public:
    const char *name() const override { return "Sync"; }
    const char *icon() const override { return "SYN"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_sync; }
    const char *slug() const override { return kSlug; }
    AppAccess access() const override { return AppAccess::Default; }
    bool needs_fresh_boot() const override { return true; }

    // True only while a transfer is live. A sweep runs 5-30 s with no button
    // presses, so without this the sleep manager suspends mid-transfer and the
    // upload fails as Lost for no reason the user can see. It must go false
    // again when idle — a permanent wake lock flattens the battery.
    // NOTE: the registry samples this once at launch (when idle), so render()
    // drives hal_sleep_wake_lock() directly off busy() and on_exit() releases
    // it. This override only covers the launch instant.
    bool wants_wake_lock() const override { return busy(); }

    // Same window. The registry's default HOME handling calls
    // app_registry_launch, which force-disables the radio — up to 2 s blocked
    // inside the LVGL lock, mid-sweep. Intercepting HOME lets us abort cleanly
    // first; once idle, HOME falls through to the registry and goes home.
    // B aborts too (see on_button), but B is purely in-app: it never leaves.
    bool handles_home_button() const override { return busy(); }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        snapshot_file_ = nullptr;
        snapshot_len_ = 0;
        profile_checked_ = profile_failed_ = false;
        ticks_ = 0;
        settle_ = 0;
        error_ticks_ = 0;
        error_what_[0] = '\0';
        error_why_[0] = '\0';
        wake_held_ = false;
        shown_state_ = XferState::Failed;   // forces the first repaint
        detail_shown_[0] = '\0';

        build_chrome();

        // The registry force-disables the radio on every app switch, so it is
        // always down on the way in here.
        if (hal_radio_enable() != ESP_OK) {
            radio_ok_ = false;
            set_border(COLOR_MAGENTA);
            set_status("Radio wouldn't start", COLOR_MAGENTA);
            set_detail("Reboot the badge");
            set_hint("HOME: home");
            return;
        }
        radio_ok_ = true;

        // A sighting from a previous visit is not evidence about now.
        bbx_xfer_reset_discovery();
        hal_radio_set_recv_cb([](const uint8_t *mac, int8_t rssi,
                                 const uint8_t *d, size_t n) {
            bbx_xfer_on_frame(mac, rssi, d, n);
        });

        enter_idle();
    }

    void on_exit() override
    {
        // Clear the callback FIRST: it is invoked from the NimBLE host task and
        // outliving this object would be a use-after-free.
        hal_radio_set_recv_cb(RadioRecvCb{});
        bbx_xfer_abort();
        free_snapshot();
        // The registry only samples wants_wake_lock() at launch (when idle),
        // so a lock taken mid-transfer must be released here and not left
        // holding the idle timer open after we are gone.
        if (wake_held_) {
            wake_held_ = false;
            hal_sleep_wake_lock(false);
        }

        // lv_obj_clean(root) in the registry destroys the widgets; only the
        // cached pointers need clearing.
        root_ = panel_ = title_ = status_ = detail_ = hint_ = nullptr;
        bar_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (!radio_ok_) return;

        // While busy() the registry does not intercept HOME (see
        // handles_home_button above), so HOME arrives here and must abort
        // cleanly exactly like B. Once idle, HOME never reaches us: the
        // registry takes it straight home.
        if (ev.button == Button::B || ev.button == Button::Home) {
            if (!busy()) return;
            bbx_xfer_abort();
            free_snapshot();
            enter_idle();
            if (ev.button == Button::Home) app_registry_return_to_launcher();
            return;
        }
        if (ev.button != Button::A) return;

        if (busy()) return;
        if (settle_ > 0) return;
        start_sync();
    }

    void on_tick() override
    {
        if (!radio_ok_) return;
        if (settle_ > 0) settle_--;
        if (error_ticks_ > 0) error_ticks_--;

        bbx_xfer_tick();
        // One-request sync: Synced is shown only after the returned profile is
        // validated and atomically applied locally. A committed backup with a
        // failed apply stays saved and reports this partial outcome for retry.
        if (!profile_checked_ && bbx_xfer_state() == XferState::Done) {
            profile_checked_ = true;
            size_t len = 0;
            const char *profile = bbx_xfer_profile(&len);
            if (!profile || hal_identity_apply_profile(profile, len) != ESP_OK) {
                profile_failed_ = true;
                show_error("Backup saved", "Profile update failed - retry sync");
            }
        }
        ticks_++;
        render();
    }

private:
    bool profile_checked_ = false, profile_failed_ = false;
    // Downlink buffer, heap-allocated only for the duration of a transfer.
    // bbx_xfer borrows it (no downlink when null); a fixed member would pin
    // ~2 KB for the app's whole lifetime.
    uint8_t *profile_response_ = nullptr;

    bool busy() const
    {
        switch (bbx_xfer_state()) {
            case XferState::Idle:
            case XferState::Done:
            case XferState::Failed:
                return false;
            default:
                return true;
        }
    }

    // CRC32 over the whole staging file (header + JSON), read a slice at a
    // time. Seed 0 is the zlib-compatible convention the station and server
    // share with bbx_xfer's RAM path.
    static uint32_t file_crc32(FILE *f)
    {
        uint8_t buf[512];
        uint32_t crc = 0;
        if (fseek(f, 0, SEEK_SET) != 0) return 0;
        for (;;) {
            size_t n = fread(buf, 1, sizeof(buf), f);
            if (n > 0) crc = esp_rom_crc32_le(crc, buf, n);
            if (n < sizeof(buf)) break;
        }
        // An empty file never happens (header alone is ~100 B); 0 also means
        // "couldn't read", and the caller treats it as a pack failure.
        return crc;
    }

    void free_snapshot()
    {
        if (snapshot_file_) {
            fclose(snapshot_file_);
            snapshot_file_ = nullptr;
        }
        unlink(kFsSnapshotTmpPath);
        // The profile is consumed (or abandoned) with the transfer; the
        // xfer layer only borrows this buffer and re-takes it on start.
        free(profile_response_);
        profile_response_ = nullptr;
    }

    void start_sync()
    {
        profile_checked_ = profile_failed_ = false;
        free_snapshot();
        error_ticks_ = 0;

        char token[65];
        if (!hal_identity_get_upload_token(token)) {
            show_error("Badge not ready to sync", "Visit registration to set up sync");
            return;
        }
        // A small transport header carries credentials separately from the JSON.
        char header[104];
        int header_len = snprintf(header, sizeof(header), "HTNSYNC1\n%s\n%s\n",
                                  hal_identity_get()->claim_id, token);
        memset(token, 0, sizeof(token));
        if (header_len < 0 || (size_t)header_len >= sizeof(header)) {
            show_error("Invalid sync identity", "Visit registration");
            return;
        }

        // Say so before the walk, not after: it reads ~40 files off LittleFS
        // synchronously under the LVGL lock and the screen is frozen for the
        // duration.
        set_border(COLOR_GOLD);
        set_status("Packing your badge...", COLOR_GOLD);
        set_detail("");
        set_hint("");
        lv_refr_now(nullptr);

        // The snapshot stages to a LittleFS file, not RAM: with the radio up
        // the heap holds ~6 KB free, so no RAM buffer can stage a transfer.
        // Packing streams the JSON to flash a slice at a time (~2 KB of stack
        // total) and the transfer reads one frame per send from the file.
        // Downlink buffer lives only for this transfer. If it won't fit,
        // degrade to upload-only (null response) rather than refusing.
        profile_response_ = (uint8_t *)malloc(BBX_PROFILE_MAX_LEN + BBX_PROFILE_PREFIX_LEN + 1);

        FsSnapshotStats st;
        size_t json_bytes = fs_snapshot_write_file(kFsSnapshotTmpPath,
                                                   (size_t)header_len,
                                                   kFsSnapshotCap, &st);
        if (json_bytes == 0) {
            memset(header, 0, sizeof(header));
            free_snapshot();
            show_error("Couldn't pack your badge", "Try again");
            return;
        }
        // Fill in the reserved header room, then checksum the whole file.
        snapshot_file_ = fopen(kFsSnapshotTmpPath, "r+b");
        if (!snapshot_file_) {
            memset(header, 0, sizeof(header));
            free_snapshot();
            show_error("Couldn't pack your badge", "Try again");
            return;
        }
        bool header_ok = fwrite(header, 1, (size_t)header_len, snapshot_file_)
                         == (size_t)header_len;
        memset(header, 0, sizeof(header));
        uint32_t crc = 0;
        if (header_ok) crc = file_crc32(snapshot_file_);
        fclose(snapshot_file_);
        snapshot_file_ = nullptr;
        if (!header_ok || crc == 0) {
            free_snapshot();
            show_error("Couldn't pack your badge", "Try again");
            return;
        }
        snapshot_len_ = (size_t)header_len + json_bytes;
        ESP_LOGI(kSlug, "snapshot %u bytes, crc %08lx, %ums, complete=%d",
                 (unsigned)json_bytes, (unsigned long)crc,
                 (unsigned)st.elapsed_ms, (int)st.complete);

        snapshot_file_ = fopen(kFsSnapshotTmpPath, "rb");
        if (!snapshot_file_) {
            free_snapshot();
            show_error("Couldn't pack your badge", "Try again");
            return;
        }
        // Each transfer read is already bounded to one frame. Avoid newlib's
        // lazy stdio buffer taking more heap just as the controller needs it.
        setvbuf(snapshot_file_, nullptr, _IONBF, 0);

        XferConfig cfg = {};
        cfg.file = snapshot_file_;
        cfg.len  = snapshot_len_;
        cfg.crc  = crc;
        cfg.kind = BBX_XFER_KIND_UPLOAD;
        cfg.enc  = BBX_XFER_ENC_RAW;
        cfg.response = profile_response_;
        cfg.response_cap = BBX_PROFILE_MAX_LEN + BBX_PROFILE_PREFIX_LEN + 1;

        if (!bbx_xfer_start(cfg)) {
            free_snapshot();
            show_error("Couldn't start the sync", "Try again");
            return;
        }
        shown_state_ = XferState::Idle;   // force a repaint on the next tick
    }

    void enter_idle()
    {
        profile_checked_ = profile_failed_ = false;
        bbx_xfer_abort();
        error_ticks_ = 0;
        error_what_[0] = '\0';
        error_why_[0] = '\0';
        set_border(COLOR_BORDER);
        shown_state_ = XferState::Failed;
        detail_shown_[0] = '\0';
    }

    void show_error(const char *what, const char *why)
    {
        free_snapshot();
        snprintf(error_what_, sizeof(error_what_), "%s", what ? what : "Sync failed");
        snprintf(error_why_, sizeof(error_why_), "%s", why ? why : "Try again");
        error_ticks_ = kErrorHoldTicks;
        set_border(COLOR_MAGENTA);
        set_status(error_what_, COLOR_MAGENTA);
        set_detail(error_why_);
        set_hint("A: try again   -   HOME: home");
        set_bar(-1);
        shown_state_ = XferState::Failed;
        settle_ = kSettleTicks;
    }

    // Repainting at 50 Hz would be wasteful and makes the screen flicker, so
    // only touch LVGL when the state or the detail string actually changes.
    void render()
    {
        // An error raised before bbx_xfer_start() leaves the transfer state at
        // Idle, so without this latch the very next tick repaints the idle
        // view over it. While the latch is held the error owns the screen.
        if (error_ticks_ > 0) {
            set_border(COLOR_MAGENTA);
            set_status(error_what_, COLOR_MAGENTA);
            set_detail(error_why_);
            set_hint("A: try again   -   HOME: home");
            set_bar(-1);
            shown_state_ = XferState::Failed;
            snprintf(detail_shown_, sizeof(detail_shown_), "%s", error_why_);
            return;
        }
        XferState st = bbx_xfer_state();
        if (st == XferState::Failed && shown_state_ == XferState::Failed) {
            // The error latch just expired: render_failed() always latches,
            // so Failed-with-no-latch means the hold ran out. Fall back to
            // the live idle view rather than freezing on stale widgets.
            bbx_xfer_abort();
            enter_idle();
            st = XferState::Idle;
        }
        bool state_changed = (st != shown_state_);
        shown_state_ = st;

        // wants_wake_lock() is sampled once at launch, when the transfer has
        // not started yet, so drive the lock directly off the live state
        // instead (same pattern as mp_session's update_wake_lock()).
        bool want_wake = busy();
        if (want_wake != wake_held_) {
            wake_held_ = want_wake;
            hal_sleep_wake_lock(want_wake);
        }

        if (profile_failed_) return;
        switch (st) {
            case XferState::Idle: render_idle(state_changed); break;
            case XferState::Opening:
                if (state_changed) {
                    set_status("Asking the station...", COLOR_GOLD);
                    set_detail("");
                    set_hint("B: cancel");
                    set_bar(-1);
                }
                pulse_border();
                break;
            case XferState::Backoff:
                if (state_changed) {
                    set_status("Station is busy", COLOR_GOLD);
                    set_hint("B: cancel");
                }
                set_detail_if_changed("someone else is syncing - waiting");
                pulse_border();
                break;
            case XferState::Sweeping:
            case XferState::Repairing: render_uploading(st, state_changed); break;
            case XferState::AwaitStat:
                if (state_changed) set_status("Uploading", COLOR_GOLD);
                set_detail_if_changed("checking what arrived");
                set_bar(bbx_xfer_progress_pct());
                pulse_border();
                break;
            case XferState::AwaitDone:
                if (state_changed) {
                    set_status("Station is sending it up...", COLOR_GOLD);
                    set_detail("");
                }
                set_bar(bbx_xfer_progress_pct());
                pulse_border();
                break;
            case XferState::Done:    render_done(state_changed);   break;
            case XferState::Failed:  render_failed(state_changed);  break;
        }
    }

    void render_idle(bool state_changed)
    {
        char id[BBX_MAX_BOX_ID + 1];
        int  rssi = 0;
        bool seen = bbx_xfer_station_seen(id, sizeof(id), &rssi);

        if (state_changed) {
            set_border(COLOR_BORDER);
            set_bar(-1);
            set_hint("A: sync now   -   HOME: home");
        }
        set_status(seen ? "Ready to sync" : "Find a sync station",
                   seen ? COLOR_ACCENT : COLOR_TEXT);

        char line[80];
        if (seen && id[0])      snprintf(line, sizeof(line), "%s   %d dBm", id, rssi);
        else if (seen)          snprintf(line, sizeof(line), "station nearby   %d dBm", rssi);
        else                    snprintf(line, sizeof(line), "looking for a station...");
        set_detail_if_changed(line);
    }

    void render_uploading(XferState st, bool state_changed)
    {
        if (state_changed) {
            set_status("Uploading", COLOR_GOLD);
            set_hint("B: cancel");
        }
        const XferStats *s = bbx_xfer_stats();
        int pct = bbx_xfer_progress_pct();
        set_bar(pct);

        char line[80];
        if (st == XferState::Repairing) {
            int missing = (int)s->total_frames - (int)s->recv_count;
            snprintf(line, sizeof(line), "filling gaps  -  %d frame%s",
                     missing, missing == 1 ? "" : "s");
        } else {
            snprintf(line, sizeof(line), "%u.%u / %u.%u KB   %d%%",
                     (unsigned)(sent_bytes(pct) / 1024),
                     (unsigned)((sent_bytes(pct) % 1024) * 10 / 1024),
                     (unsigned)(snapshot_len_ / 1024),
                     (unsigned)((snapshot_len_ % 1024) * 10 / 1024), pct);
        }
        set_detail_if_changed(line);
        pulse_border();
    }

    size_t sent_bytes(int pct) const
    {
        if (pct <= 0) return 0;
        size_t n = snapshot_len_ * (size_t)pct / 80;   // the sweep owns 0..80%
        return n > snapshot_len_ ? snapshot_len_ : n;
    }

    void render_done(bool state_changed)
    {
        if (!state_changed) return;
        const XferStats *s = bbx_xfer_stats();
        set_border(COLOR_GREEN);
        set_status("Synced", COLOR_GREEN);
        set_bar(100);
        char line[80];
        snprintf(line, sizeof(line), "%u.%u KB  -  %u.%us  -  %u repair%s",
                 (unsigned)(snapshot_len_ / 1024),
                 (unsigned)((snapshot_len_ % 1024) * 10 / 1024),
                 (unsigned)(s->elapsed_ms / 1000), (unsigned)((s->elapsed_ms / 100) % 10),
                 (unsigned)s->repair_rounds, s->repair_rounds == 1 ? "" : "s");
        set_detail(line);
        set_hint("A: sync again   -   HOME: home");
        free_snapshot();
        settle_ = kSettleTicks;
    }

    void render_failed(bool state_changed)
    {
        if (!state_changed) return;

        const char *what = "Sync failed";
        const char *why  = "Try again";
        switch (bbx_xfer_fail()) {
            case XferFail::Radio:
                what = "Radio wouldn't start";      why = "Reboot the badge"; break;
            case XferFail::NoStation:
                what = "No sync station in range";  why = "Move closer to the sync desk"; break;
            case XferFail::StationBusy:
                what = "Station is busy";           why = "Someone else is syncing - try again"; break;
            case XferFail::Lost:
                what = "Lost the station";          why = "Stay near it and try again"; break;
            case XferFail::Stalled:
                what = "Signal too weak";           why = "Move closer, or sync via hacker dashboard"; break;
            case XferFail::Crc:
                what = "Transfer got corrupted";    why = "Try again"; break;
            case XferFail::TooBig:
                what = "Backup is too large";       why = "Tell a volunteer"; break;
            case XferFail::Server:
                // The badge only learns the coarse station verdict, not the
                // endpoint's numeric code (the desk screen shows HTTP nnn).
                // A lost response can leave a committed backup behind, so a
                // failed upload still asks for a retry rather than a rollback.
                what = "Sync didn't finish";
                switch (bbx_xfer_server_status()) {
                    case BBX_ST_NO_WIFI:   why = "The station is offline - try again"; break;
                    case BBX_ST_TRANSPORT: why = "No answer - backup may be saved, retry"; break;
                    case BBX_ST_HTTP:      why = "The server refused it - see a volunteer"; break;
                    default:               why = "Try again"; break;
                }
                break;
            case XferFail::None:
            default:
                // Reached on the way in, before anything has been attempted.
                render_idle(true);
                return;
        }
        show_error(what, why);
    }

    void pulse_border()
    {
        int phase = ticks_ % PULSE_PERIOD;
        set_border(phase < PULSE_PERIOD / 2 ? COLOR_GOLD : COLOR_BORDER);
    }

    // --- chrome ------------------------------------------------------------

    void build_chrome()
    {
        title_ = lv_label_create(root_);
        lv_label_set_text(title_, "Sync");
        lv_obj_set_style_text_color(title_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_24, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 10);

        panel_ = lv_obj_create(root_);
        lv_obj_remove_style_all(panel_);
        lv_obj_set_size(panel_, 280, 132);
        lv_obj_align(panel_, LV_ALIGN_CENTER, 0, 4);
        lv_obj_set_style_bg_color(panel_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(panel_, 0, 0);
        lv_obj_set_style_border_width(panel_, 2, 0);
        lv_obj_set_style_border_color(panel_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_clear_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);

        status_ = lv_label_create(panel_);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(status_, 252);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -26);

        detail_ = lv_label_create(panel_);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(detail_, 252);
        lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(detail_, &lv_font_montserrat_14, 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 8);

        // A percentage in text is hard to read at a glance while holding a
        // badge still; the bar is what tells someone whether to keep waiting.
        bar_ = lv_bar_create(panel_);
        lv_obj_set_size(bar_, 236, 6);
        lv_obj_align(bar_, LV_ALIGN_CENTER, 0, 40);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar_, 0, 0);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(COLOR_GOLD), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar_, 0, LV_PART_INDICATOR);
        lv_bar_set_range(bar_, 0, 100);
        lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);

        hint_ = lv_label_create(root_);
        lv_obj_set_style_text_color(hint_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -8);

    }

    void set_status(const char *s, uint32_t color)
    {
        if (!status_) return;
        lv_label_set_text(status_, s);
        lv_obj_set_style_text_color(status_, lv_color_hex(color), 0);
    }

    void set_detail(const char *s)
    {
        if (detail_) lv_label_set_text(detail_, s ? s : "");
        snprintf(detail_shown_, sizeof(detail_shown_), "%s", s ? s : "");
    }

    void set_detail_if_changed(const char *s)
    {
        if (strcmp(detail_shown_, s) == 0) return;
        set_detail(s);
    }

    void set_hint(const char *s)
    {
        if (hint_) lv_label_set_text(hint_, s ? s : "");
    }

    void set_border(uint32_t color)
    {
        if (panel_) lv_obj_set_style_border_color(panel_, lv_color_hex(color), 0);
    }

    // pct < 0 hides the bar.
    void set_bar(int pct)
    {
        if (!bar_) return;
        if (pct < 0) {
            lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(bar_, pct, LV_ANIM_OFF);
    }

    lv_obj_t *root_        = nullptr;
    lv_obj_t *title_       = nullptr;
    lv_obj_t *panel_       = nullptr;
    lv_obj_t *status_      = nullptr;
    lv_obj_t *detail_      = nullptr;
    lv_obj_t *hint_        = nullptr;
    lv_obj_t *bar_         = nullptr;

    FILE     *snapshot_file_ = nullptr;  // staging file, open "rb" while live
    size_t    snapshot_len_ = 0;          // header + JSON on the wire
    bool      radio_ok_     = false;
    bool      wake_held_    = false;
    int       ticks_        = 0;
    int       settle_       = 0;
    int       error_ticks_  = 0;
    char      error_what_[64] = {};
    char      error_why_[80] = {};
    XferState shown_state_  = XferState::Idle;
    char      detail_shown_[80] = {};
};

static SyncApp s_instance;

App *app_sync_instance() { return &s_instance; }
