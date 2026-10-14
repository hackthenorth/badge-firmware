#include "app.h"
#include "app_registry.h"
#include "app_share_proto.h"

extern "C" const lv_image_dsc_t icon_share;
#include "launcher_slugs.h"
#include "script_app.h"
#include "share_bundle.h"
#include "share_xfer.h"
#include "hal_identity.h"
#include "hal_radio.h"
#include "hal_sleep.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Share: send an installed Lua app to a nearby badge.
// Send: pick an app -> offer -> receiver accepts -> stream -> done.
// Receive: listen -> review offer -> A accepts -> stream -> install.
// No physical bump or synchronized button presses are needed.
// The engine owns radio/protocol; this app owns packing, prompts and installs.
// The install path mirrors boot: ScriptApp::load is the single semantic
// validator, then app_registry_add + launcher_slugs_append, which the
// launcher picks up live (it resolves slugs on every refresh).

static constexpr uint32_t COLOR_BG = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_FOCUS = BadgeTheme::SURFACE;
static constexpr uint32_t COLOR_BORDER = BadgeTheme::BORDER;
static constexpr uint32_t COLOR_ACCENT = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_TEXT = BadgeTheme::TEXT;
static constexpr uint32_t COLOR_DIM = BadgeTheme::TEXT_MUTED;
static constexpr uint32_t COLOR_BAD = 0xff5c8a;
static constexpr uint32_t COLOR_GOOD = 0x36e07a;
static constexpr uint32_t COLOR_GOLD = 0xffd166;

static const char *kAppsDir = "/littlefs/apps";
static const char *kSendBundle = "/littlefs/apps/.share-tx.bin";
static const char *kReceiveBundle = "/littlefs/apps/.share-rx.bin";
static const char *kStagingDir = "/littlefs/apps/.rx";

struct SendItem {
    std::string slug;
    std::string name;
    std::string author;
    std::string version;
    std::string dir;
    size_t size = 0;
};

const char *fail_text(ShareFail f)
{
    switch (f) {
    case ShareFail::None:      return "Done";
    case ShareFail::Radio:     return "Radio wouldn't start";
    case ShareFail::Declined:  return "Declined";
    case ShareFail::Cancelled: return "Cancelled";
    case ShareFail::Lost:      return "Lost each other";
    case ShareFail::Stalled:   return "Signal too weak";
    case ShareFail::Crc:       return "Transfer got corrupted";
    case ShareFail::TooBig:    return "App is too big to share";
    case ShareFail::NoSpace:   return "Not enough memory";
    case ShareFail::StorageFull: return "Storage is full";
    case ShareFail::Storage:   return "Couldn't access app files";
    }
    return "Failed";
}

const char *fail_hint(ShareFail f)
{
    switch (f) {
    case ShareFail::Radio:     return "Reboot the badge and try again";
    case ShareFail::Lost:
    case ShareFail::Stalled:   return "Hold the badges close and still";
    case ShareFail::Crc:       return "Try again";
    case ShareFail::TooBig:    return "Share supports apps up to 48 KiB";
    case ShareFail::NoSpace:   return "Reopen Share and try again";
    case ShareFail::StorageFull: return "Free some storage and try again";
    case ShareFail::Storage:   return "Try again; check the console if it persists";
    default:                   return "";
    }
}

class ShareApp final : public App {
public:
    const char *name() const override { return "Share"; }
    const char *icon() const override { return "SHR"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_share; }
    const char *slug() const override { return "share"; }
    AppAccess access() const override { return AppAccess::Default; }
    bool needs_fresh_boot() const override { return true; }
    bool wants_wake_lock() const override { return busy(); }
    bool handles_home_button() const override { return busy(); }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        reset_run_state();
        enter_chooser();
    }

    void on_exit() override
    {
        share_xfer_abort();
        free_bundle();
        share_bundle_remove_dir(kStagingDir);   // abandoned conflict prompt
        send_list_.clear();
        if (wake_held_) {
            wake_held_ = false;
            hal_sleep_wake_lock(false);
        }
        root_ = nullptr;
        title_ = status_ = detail_ = hint_ = bar_ = nullptr;
        for (auto &r : rows_) r = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed || !root_) return;
        if (ev.button == Button::Home && busy()) {
            // Intercepted only while busy (handles_home_button); abort first
            // so no burst is left on the air, then go home like Sync does.
            abort_to_chooser();
            app_registry_return_to_launcher();
            return;
        }
        switch (mode_) {
        case Mode::Chooser:    chooser_button(ev); break;
        case Mode::SendList:   list_button(ev); break;
        case Mode::SendDetail: detail_button(ev); break;
        case Mode::SendActive: active_button(ev); break;
        case Mode::RecvWait:   recv_wait_button(ev); break;
        case Mode::Offer:      offer_button(ev); break;
        case Mode::Conflict:   conflict_button(ev); break;
        case Mode::RecvActive: active_button(ev); break;
        case Mode::Result:     result_button(ev); break;
        }
    }

    void on_tick() override
    {
        if (!root_) return;
        drive_wake_lock();

        if (share_xfer_role() != ShareRole::Idle) share_xfer_tick();

        // Engine transitions drive the UI.
        if (mode_ == Mode::RecvWait && share_xfer_state() == ShareState::OfferReady) {
            if (capture_offer()) enter_offer();
        } else if (mode_ == Mode::Offer && share_xfer_state() != ShareState::OfferReady) {
            // Latch expired while the prompt was up.
            enter_recv_wait();
        } else if ((mode_ == Mode::SendActive || mode_ == Mode::RecvActive)
                   && share_xfer_state() == ShareState::Done) {
            on_engine_done();
        } else if ((mode_ == Mode::SendActive || mode_ == Mode::RecvActive)
                   && share_xfer_state() == ShareState::Failed) {
            enter_result(false, fail_text(share_xfer_fail()),
                         fail_hint(share_xfer_fail()));
        }

        // Live progress without full rebuilds.
        if ((mode_ == Mode::SendActive || mode_ == Mode::RecvActive) && bar_) {
            ShareState xs = share_xfer_state();
            if (xs == ShareState::Sweeping || xs == ShareState::Repairing
                || xs == ShareState::Receiving || xs == ShareState::Accepting
                || xs == ShareState::Verifying || xs == ShareState::DoneHold) {
                lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
                if (pct_) lv_obj_clear_flag(pct_, LV_OBJ_FLAG_HIDDEN);
            }
            int pct = share_xfer_progress_pct();
            if (pct != shown_pct_) {
                shown_pct_ = pct;
                lv_bar_set_value(bar_, pct, LV_ANIM_OFF);
                if (pct_) lv_label_set_text_fmt(pct_, "%d%%", pct);
            }
            const char *phase = send_phase_text();
            if (phase && status_ && phase != last_phase_) {
                last_phase_ = phase;
                lv_label_set_text(status_, phase);
            }
        }
    }

private:
    enum class Mode {
        Chooser, SendList, SendDetail, SendActive,
        RecvWait, Offer, Conflict, RecvActive, Result,
    };

    bool busy() const
    {
        if (share_xfer_role() == ShareRole::Idle) return false;
        ShareState s = share_xfer_state();
        return s != ShareState::Idle && s != ShareState::Done
            && s != ShareState::Failed;
    }

    void drive_wake_lock()
    {
        // wants_wake_lock() is sampled once at launch (when idle), so drive
        // the lock directly off the live state like Sync does.
        bool want = busy();
        if (want != wake_held_) {
            wake_held_ = want;
            hal_sleep_wake_lock(want);
        }
    }

    void reset_run_state()
    {
        mode_ = Mode::Chooser;
        chooser_sel_ = 0;
        sel_ = 0;
        scroll_ = 0;
        conflict_sel_ = 0;
        shown_pct_ = -1;
        last_phase_ = nullptr;
        free_bundle();
        send_list_.clear();
        offer_slug_[0] = offer_name_[0] = offer_ver_[0] = '\0';
        offer_len_ = 0;
        offer_refused_ = false;
        conflict_decided_ = false;
        conflict_overwrite_ = false;
        result_title_.clear();
        result_hint_.clear();
    }

    void free_bundle()
    {
        unlink(kSendBundle);
    }

    void abort_to_chooser()
    {
        share_xfer_abort();
        free_bundle();
        enter_chooser();
    }

    // ---- send list ----

    void read_manifest_text(const char *dir, SendItem &item)
    {
        // Precision-capped: 140 + 13 + NUL < 160 by construction, so this
        // cannot trip -Werror=format-truncation.
        char path[160];
        snprintf(path, sizeof(path), "%.140s/manifest.cfg", dir);
        FILE *f = fopen(path, "r");
        if (!f) return;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            // Trim trailing space on the key and leading space on the value;
            // keep it small — display only, the loader does strict parsing.
            char *kend = eq - 1;
            while (kend > line && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';
            char *v = eq + 1;
            while (*v == ' ' || *v == '\t') v++;
            char *vend = v + strlen(v);
            while (vend > v && (vend[-1] == '\n' || vend[-1] == '\r'
                                || vend[-1] == ' ' || vend[-1] == '\t')) *--vend = '\0';
            if (strcmp(line, "name") == 0) item.name = v;
            else if (strcmp(line, "author") == 0) item.author = v;
            else if (strcmp(line, "version") == 0) item.version = v;
        }
        fclose(f);
    }

    void build_send_list()
    {
        send_list_.clear();
        for (size_t i = 0; i < app_registry_count(); i++) {
            App *a = app_registry_get(i);
            if (!a || !a->is_user_installed() || !a->slug()) continue;
            SendItem item;
            item.slug = a->slug();
            item.name = a->name() ? a->name() : a->slug();
            // "/littlefs/apps/" + 32-char max slug + NUL < 96 by construction.
            char dir[96];
            snprintf(dir, sizeof(dir), "%s/%.32s", kAppsDir, a->slug());
            item.dir = dir;
            read_manifest_text(dir, item);
            item.size = share_bundle_dir_size(dir);
            send_list_.push_back(item);
        }
    }

    // ---- mode entry ----

    void clear_chrome()
    {
        if (root_) lv_obj_clean(root_);
        title_ = status_ = detail_ = hint_ = bar_ = pct_ = nullptr;
        for (auto &r : rows_) r = nullptr;
        shown_pct_ = -1;
        last_phase_ = nullptr;
    }

    void make_title(const char *text)
    {
        title_ = lv_label_create(root_);
        lv_label_set_text(title_, text);
        lv_obj_set_style_text_color(title_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_20, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 6);
    }

    void make_hint(const char *text)
    {
        hint_ = lv_label_create(root_);
        lv_obj_set_style_text_color(hint_, lv_color_hex(COLOR_DIM), 0);
        lv_label_set_text(hint_, text);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    void make_progress()
    {
        bar_ = lv_bar_create(root_);
        lv_obj_set_size(bar_, 236, 8);
        lv_obj_align(bar_, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(bar_, lv_color_hex(COLOR_ACCENT), LV_PART_INDICATOR);
        lv_bar_set_range(bar_, 0, 100);
        lv_bar_set_value(bar_, 0, LV_ANIM_OFF);
        pct_ = lv_label_create(root_);
        lv_obj_set_style_text_color(pct_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(pct_, &lv_font_montserrat_18, 0);
        lv_label_set_text(pct_, "0%");
        lv_obj_align(pct_, LV_ALIGN_CENTER, 0, 52);
    }

    void enter_chooser()
    {
        share_xfer_abort();
        free_bundle();
        mode_ = Mode::Chooser;
        chooser_sel_ = 0;
        clear_chrome();
        make_title("Share Custom Apps");
        for (int i = 0; i < 2; i++) {
            lv_obj_t *row = lv_label_create(root_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_18, 0);
            lv_obj_set_pos(row, 16, 76 + i * 46);
            lv_obj_set_width(row, 320 - 32);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            rows_[i] = row;
        }
        make_hint("Up/Down: choose   A: select");
        render_chooser();
    }

    void render_chooser()
    {
        const char *texts[2] = { " Send an app", " Receive an app" };
        for (int i = 0; i < 2; i++) {
            if (!rows_[i]) continue;
            lv_label_set_text(rows_[i], texts[i]);
            bool focused = (i == chooser_sel_);
            lv_obj_set_style_bg_color(rows_[i], lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(rows_[i], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(rows_[i],
                                        lv_color_hex(focused ? COLOR_ACCENT : COLOR_TEXT), 0);
        }
    }

    void enter_send_list()
    {
        share_xfer_abort();
        free_bundle();
        mode_ = Mode::SendList;
        sel_ = 0;
        scroll_ = 0;
        clear_chrome();
        make_title("Send an app");
        build_send_list();
        if (send_list_.empty()) {
            status_ = lv_label_create(root_);
            lv_label_set_text(status_, "No custom apps yet");
            lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_DIM), 0);
            lv_obj_set_style_text_font(status_, &lv_font_montserrat_18, 0);
            lv_obj_align(status_, LV_ALIGN_CENTER, 0, -10);
            detail_ = lv_label_create(root_);
            lv_label_set_text(detail_, "Install one with the console:\nput + reload");
            lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
            lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 24);
            make_hint("B: back");
            return;
        }
        for (int i = 0; i < 5; i++) {
            lv_obj_t *row = lv_label_create(root_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_16, 0);
            lv_obj_set_pos(row, 12, 44 + i * 32);
            lv_obj_set_width(row, 320 - 24);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            rows_[i] = row;
        }
        make_hint("Up/Down: pick   A: send   B: back");
        render_list();
    }

    void render_list()
    {
        if (send_list_.empty()) return;
        if (sel_ < scroll_) scroll_ = sel_;
        if (sel_ > scroll_ + 4) scroll_ = sel_ - 4;
        for (int i = 0; i < 5; i++) {
            if (!rows_[i]) continue;
            size_t idx = scroll_ + i;
            if (idx >= send_list_.size()) {
                lv_label_set_text(rows_[i], "");
                lv_obj_set_style_bg_opa(rows_[i], LV_OPA_TRANSP, 0);
                continue;
            }
            const SendItem &it = send_list_[idx];
            // Capped precisions keep every expansion under 128 by construction.
            char line[128];
            if (it.author.empty()) {
                snprintf(line, sizeof(line), " %.100s", it.name.c_str());
            } else {
                snprintf(line, sizeof(line), " %.70s (%.40s)", it.name.c_str(),
                         it.author.c_str());
            }
            lv_label_set_text(rows_[i], line);
            bool focused = (idx == sel_);
            lv_obj_set_style_bg_color(rows_[i], lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(rows_[i], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(rows_[i],
                                        lv_color_hex(focused ? COLOR_ACCENT : COLOR_TEXT), 0);
        }
    }

    void enter_send_detail()
    {
        if (sel_ >= send_list_.size()) {
            enter_send_list();
            return;
        }
        mode_ = Mode::SendDetail;
        clear_chrome();
        make_title("Send this app?");
        const SendItem &it = send_list_[sel_];
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, it.name.c_str());
        lv_obj_set_width(status_, 296);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(status_, 12, 48);
        detail_ = lv_label_create(root_);
        // kb[16] holds any %u ("4294967295 KB" is 13 + NUL) by construction.
        char kb[16];
        if (it.size < 1024) snprintf(kb, sizeof(kb), "%u B", (unsigned)it.size);
        else snprintf(kb, sizeof(kb), "%u KB", (unsigned)(it.size / 1024));
        char meta[120];
        if (it.version.empty()) {
            snprintf(meta, sizeof(meta), "%.40s  -  %s", it.slug.c_str(), kb);
        } else {
            snprintf(meta, sizeof(meta), "%s  -  v%.60s", kb, it.version.c_str());
        }
        lv_label_set_text(detail_, meta);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 6);
        make_hint("A: offer app   B: back");
    }

    void start_sending()
    {
        // Pause any previous offer while preparing the bundle. Keep the radio
        // session enabled so the registry can reboot on exit to reclaim RAM.
        share_xfer_abort();
        const SendItem &it = send_list_[sel_];
        char why[160] = {};
        size_t len = 0;
        uint32_t crc = 0;
        if (!share_bundle_pack_file(it.dir.c_str(), kSendBundle, &len, &crc, why, sizeof(why))) {
            enter_result(false, "Can't pack that app", why[0] ? why : "Try again");
            return;
        }
        if (!share_xfer_start_send(kSendBundle, len, crc, it.slug.c_str(),
                                   it.name.c_str(), it.version.c_str())) {
            enter_result(false, "Couldn't start sharing", "Try again");
            return;
        }
        if (share_xfer_state() == ShareState::Failed) {
            enter_result(false, fail_text(share_xfer_fail()),
                         fail_hint(share_xfer_fail()));
            return;
        }
        mode_ = Mode::SendActive;
        clear_chrome();
        make_title("Offering app");
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, "WAIT FOR ACCEPTANCE");
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -34);
        detail_ = lv_label_create(root_);
        lv_label_set_text(detail_, it.name.c_str());
        lv_obj_set_width(detail_, 296);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, -6);
        make_progress();
        if (bar_) lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        if (pct_) lv_obj_add_flag(pct_, LV_OBJ_FLAG_HIDDEN);
        make_hint("Accept on the receiving badge\nKeep badges close   B: cancel");
    }

    const char *send_phase_text()
    {
        if (share_xfer_role() == ShareRole::Receiver) {
            if (share_xfer_state() == ShareState::Verifying) return "VERIFYING";
            if (share_xfer_state() == ShareState::DoneHold) return "RECEIVED!";
            return nullptr;
        }
        switch (share_xfer_state()) {
        case ShareState::Offering:  return "WAIT FOR ACCEPTANCE";
        case ShareState::Sweeping:
        case ShareState::Repairing: return "SENDING";
        case ShareState::DoneHold:
        case ShareState::Done:      return "SENT!";
        default:                    return nullptr;
        }
    }

    void enter_recv_wait()
    {
        share_xfer_abort();
        mode_ = Mode::RecvWait;
        clear_chrome();
        make_title("Receive an app");
        if (!share_xfer_start_listen(kReceiveBundle)
            || share_xfer_state() == ShareState::Failed) {
            enter_result(false, fail_text(share_xfer_fail()),
                         fail_hint(share_xfer_fail()));
            return;
        }
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, "WAITING...");
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_22, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -12);
        detail_ = lv_label_create(root_);
        lv_label_set_text(detail_, "Ask them to send,\nthen accept here");
        lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 28);
        make_hint("B: back");
    }

    bool capture_offer()
    {
        size_t len = 0;
        uint32_t crc = 0;
        int rssi = 127;
        if (!share_xfer_offer(offer_slug_, sizeof(offer_slug_),
                              offer_name_, sizeof(offer_name_),
                              offer_ver_, sizeof(offer_ver_),
                              &len, &crc, &rssi)) return false;
        offer_len_ = len;
        return true;
    }

    void enter_offer()
    {
        mode_ = Mode::Offer;
        clear_chrome();
        make_title("Incoming app");
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, offer_name_[0] ? offer_name_ : offer_slug_);
        lv_obj_set_width(status_, 296);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_22, 0);
        lv_obj_set_pos(status_, 12, 48);
        detail_ = lv_label_create(root_);
        char kb[16];
        if (offer_len_ < 1024) snprintf(kb, sizeof(kb), "%u B", (unsigned)offer_len_);
        else snprintf(kb, sizeof(kb), "%u KB", (unsigned)(offer_len_ / 1024));
        // Built-in collision is refused before accepting: a built-in
        // slug can never be installed, so accepting would waste the transfer.
        App *existing = app_registry_find_by_slug(offer_slug_);
        if (existing && !existing->is_user_installed()) {
            lv_label_set_text(detail_, "Conflicts with a built-in app.\nAsk staff for help.");
            lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_BAD), 0);
            lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 10);
            make_hint("B: back");
            offer_refused_ = true;
            return;
        }
        offer_refused_ = false;
        char meta[160];
        if (existing) {
            snprintf(meta, sizeof(meta), "%s  -  you already have this", kb);
        } else if (offer_ver_[0]) {
            snprintf(meta, sizeof(meta), "%s  -  v%.100s", kb, offer_ver_);
        } else {
            snprintf(meta, sizeof(meta), "%s", kb);
        }
        lv_label_set_text(detail_, meta);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 10);
        make_hint("A: accept   B: decline");
    }

    void accept_current_offer()
    {
        const BadgeIdentity *id = hal_identity_get();
        char who[40];
        if (hal_identity_is_provisioned() && id->display_name[0]) {
            snprintf(who, sizeof(who), "%s", id->display_name);
        } else {
            uint8_t mac[6] = {};
            hal_radio_local_mac(mac);
            snprintf(who, sizeof(who), "Badge %02X%02X", mac[4], mac[5]);
        }
        if (!share_xfer_accept_offer(who)) {
            enter_recv_wait();   // latch expired under the prompt
            return;
        }
        if (share_xfer_state() == ShareState::Failed) {
            enter_result(false, fail_text(share_xfer_fail()),
                         fail_hint(share_xfer_fail()));
            return;
        }
        enter_recv_active();
    }

    void enter_recv_active()
    {
        mode_ = Mode::RecvActive;
        clear_chrome();
        make_title("Receiving");
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, offer_name_[0] ? offer_name_ : offer_slug_);
        lv_obj_set_width(status_, 296);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_18, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -34);
        make_progress();
        shown_pct_ = -1;
        make_hint("Hold still   -   B: cancel");
    }

    void enter_conflict()
    {
        mode_ = Mode::Conflict;
        conflict_sel_ = 0;
        conflict_decided_ = false;
        conflict_overwrite_ = false;
        clear_chrome();
        make_title("Already installed");
        detail_ = lv_label_create(root_);
        char line[120];
        snprintf(line, sizeof(line), "'%s' is already here.",
                 offer_name_[0] ? offer_name_ : offer_slug_);
        lv_label_set_text(detail_, line);
        lv_obj_set_width(detail_, 296);
        lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_align(detail_, LV_ALIGN_CENTER, 0, -30);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *row = lv_label_create(root_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_18, 0);
            lv_obj_set_pos(row, 16, 108 + i * 38);
            lv_obj_set_width(row, 320 - 32);
            rows_[i] = row;
        }
        make_hint("Up/Down: pick   A: do it");
        render_conflict();
    }

    void render_conflict()
    {
        const char *texts[3] = { " Keep both", " Overwrite mine", " Cancel" };
        for (int i = 0; i < 3; i++) {
            if (!rows_[i]) continue;
            lv_label_set_text(rows_[i], texts[i]);
            bool focused = (i == conflict_sel_);
            lv_obj_set_style_bg_color(rows_[i], lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(rows_[i], focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(rows_[i],
                                        lv_color_hex(focused ? COLOR_ACCENT : COLOR_TEXT), 0);
        }
    }

    void enter_result(bool ok, const char *title, const char *hint)
    {
        share_xfer_abort();
        free_bundle();
        mode_ = Mode::Result;
        result_title_ = title ? title : "";
        result_hint_ = hint ? hint : "";
        clear_chrome();
        make_title(ok ? "Done" : "Not shared");
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, result_title_.c_str());
        lv_obj_set_width(status_, 296);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(ok ? COLOR_GOOD : COLOR_BAD), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, -12);
        if (!result_hint_.empty()) {
            detail_ = lv_label_create(root_);
            lv_label_set_text(detail_, result_hint_.c_str());
            lv_obj_set_width(detail_, 296);
            lv_label_set_long_mode(detail_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(detail_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(detail_, lv_color_hex(COLOR_DIM), 0);
            lv_obj_align(detail_, LV_ALIGN_CENTER, 0, 30);
        }
        make_hint("A: share more   HOME: home");
    }

    void on_engine_done()
    {
        if (share_xfer_role() == ShareRole::Sender) {
            char line[120];
            if (sel_ < send_list_.size()) {
                snprintf(line, sizeof(line), "Sent '%.100s'",
                         send_list_[sel_].name.c_str());
            } else {
                snprintf(line, sizeof(line), "Sent!");
            }
            enter_result(true, line, "They still need to install it");
            return;
        }
        // Receiver: install synchronously. Say so first — unpack + rename +
        // load does LittleFS IO under the LVGL lock and the screen freezes.
        mode_ = Mode::RecvActive;
        clear_chrome();
        make_title("Installing");
        status_ = lv_label_create(root_);
        lv_label_set_text(status_, "Writing...");
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_GOLD), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_20, 0);
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, 0);
        lv_refr_now(nullptr);
        install_received();
    }

    void install_received()
    {
        size_t len = 0;
        const char *bundle_path = share_xfer_rx_file(&len);
        if (!bundle_path || len == 0) {
            enter_result(false, "Nothing arrived", "Try again");
            return;
        }
        // The engine has stopped advertising. Stop callbacks while installing,
        // but keep the session flag for the registry's reboot-on-exit rule.
        hal_radio_set_recv_cb(RadioRecvCb{});
        char why[160] = {};
        share_bundle_remove_dir(kStagingDir);
        if (!share_bundle_unpack_file(bundle_path, kStagingDir, why, sizeof(why))) {
            enter_result(false, "App didn't survive the trip",
                         why[0] ? why : "Try again");
            return;
        }
        // Unpacking is complete; remove the engine's temporary bundle.
        share_xfer_abort();
        char slug[33] = {};
        char manifest[200];
        snprintf(manifest, sizeof(manifest), "%s/manifest.cfg", kStagingDir);
        if (!share_bundle_slug_from_manifest(manifest, slug, sizeof(slug))) {
            share_bundle_remove_dir(kStagingDir);
            enter_result(false, "App has no usable name", "Tell the sender");
            return;
        }
        App *existing = app_registry_find_by_slug(slug);
        if (existing && !existing->is_user_installed()) {
            share_bundle_remove_dir(kStagingDir);
            enter_result(false, "Conflicts with a built-in app",
                         "Ask staff for help");
            return;
        }
        if (existing && !conflict_decided_) {
            // Decided after the transfer on purpose: the prompt needs the
            // real app name, and losing a transfer to a prompt timeout would
            // be worse than downloading first.
            enter_conflict();
            return;
        }
        finish_install(slug, existing);
    }

    void finish_install(const char *slug, App *existing)
    {
        bool overwrite = (existing != nullptr) && conflict_overwrite_;
        char final_dir[96];
        char final_slug[33];
        snprintf(final_slug, sizeof(final_slug), "%s", slug);
        if (existing && !overwrite) {
            // Keep both: find <slug>-N and patch the staged manifest so the
            // folder still equals the slug, which the loader requires.
            int n = 2;
            for (; n < 100; n++) {
                snprintf(final_slug, sizeof(final_slug), "%.24s-%d", slug, n);
                if (!share_bundle_valid_slug(final_slug)) break;
                snprintf(final_dir, sizeof(final_dir), "%s/%s", kAppsDir, final_slug);
                struct stat st;
                if (stat(final_dir, &st) != 0
                    && !app_registry_find_by_slug(final_slug)) break;
            }
            if (n >= 100) {
                share_bundle_remove_dir(kStagingDir);
                enter_result(false, "Too many copies here", "Delete one first");
                return;
            }
            char manifest[200];
            snprintf(manifest, sizeof(manifest), "%s/manifest.cfg", kStagingDir);
            if (!share_bundle_patch_slug(manifest, final_slug)) {
                share_bundle_remove_dir(kStagingDir);
                enter_result(false, "Couldn't rename the copy", "Try again");
                return;
            }
        } else {
            snprintf(final_dir, sizeof(final_dir), "%s/%s", kAppsDir, slug);
            if (overwrite) {
                // Rotate aside, don't delete: if the rename or load below
                // fails, the old app (and its saves) must survive. The old
                // registry object stays live until the new load succeeds.
                char backup[112];
                snprintf(backup, sizeof(backup), "%s.bak", final_dir);
                share_bundle_remove_dir(backup);
                if (rename(final_dir, backup) != 0) {
                    share_bundle_remove_dir(kStagingDir);
                    enter_result(false, "Couldn't install the app",
                                 "Storage may be full");
                    return;
                }
                if (rename(kStagingDir, final_dir) != 0) {
                    rename(backup, final_dir);   // put it all back
                    enter_result(false, "Couldn't install the app",
                                 "Storage may be full");
                    return;
                }
                ScriptApp *app = ScriptApp::load(final_dir);
                if (!app) {
                    share_bundle_remove_dir(final_dir);
                    rename(backup, final_dir);   // old app survives intact
                    enter_result(false, "App failed validation",
                                 "Your old copy is untouched");
                    return;
                }
                // New app is good: drop the old registration, carry the old
                // saves forward (bundles never contain appdata), bury backup.
                app_registry_remove(existing);
                char old_data[128], new_data[128];
                snprintf(old_data, sizeof(old_data), "%s/appdata", backup);
                snprintf(new_data, sizeof(new_data), "%s/appdata", final_dir);
                struct stat st;
                if (stat(old_data, &st) == 0 && S_ISDIR(st.st_mode)
                    && stat(new_data, &st) != 0) {
                    rename(old_data, new_data);
                }
                share_bundle_remove_dir(backup);
                app_registry_add(app);
                launcher_slugs_append(app->slug());
                char line[140];
                snprintf(line, sizeof(line), "Added '%.90s'",
                         app->name());
                enter_result(true, line, "Find it in your launcher");
                return;
            }
        }
        if (rename(kStagingDir, final_dir) != 0) {
            share_bundle_remove_dir(kStagingDir);
            enter_result(false, "Couldn't install the app", "Storage may be full");
            return;
        }
        ScriptApp *app = ScriptApp::load(final_dir);
        if (!app) {
            share_bundle_remove_dir(final_dir);
            enter_result(false, "App failed validation", "Tell the sender");
            return;
        }
        app_registry_add(app);
        launcher_slugs_append(app->slug());
        char line[140];
        snprintf(line, sizeof(line), "Added '%.90s'",
                 app->name());
        enter_result(true, line, "Find it in your launcher");
    }

    // ---- buttons ----

    void chooser_button(ButtonEvent ev)
    {
        switch (ev.button) {
        case Button::Up:
            if (chooser_sel_ > 0) {
                chooser_sel_--;
                render_chooser();
            }
            break;
        case Button::Down:
            if (chooser_sel_ < 1) {
                chooser_sel_++;
                render_chooser();
            }
            break;
        case Button::A:
        case Button::Right:
        case Button::Start:
            if (chooser_sel_ == 0) enter_send_list();
            else enter_recv_wait();
            break;
        default:
            break;
        }
    }

    void list_button(ButtonEvent ev)
    {
        if (send_list_.empty()) {
            if (ev.button == Button::B || ev.button == Button::Left) enter_chooser();
            return;
        }
        switch (ev.button) {
        case Button::Up:
            if (sel_ > 0) {
                sel_--;
                render_list();
            }
            break;
        case Button::Down:
            if (sel_ + 1 < send_list_.size()) {
                sel_++;
                render_list();
            }
            break;
        case Button::A:
        case Button::Right:
        case Button::Start:
            enter_send_detail();
            break;
        case Button::B:
        case Button::Left:
            enter_chooser();
            break;
        default:
            break;
        }
    }

    void detail_button(ButtonEvent ev)
    {
        switch (ev.button) {
        case Button::A:
        case Button::Start:
            start_sending();
            break;
        case Button::B:
        case Button::Left:
            enter_send_list();
            break;
        default:
            break;
        }
    }

    void active_button(ButtonEvent ev)
    {
        switch (ev.button) {
        case Button::B:
        case Button::Left:
            // Cancel the run and go quiet; an active peer times out.
            // An unaccepted sender keeps offering until its user cancels.
            abort_to_chooser();
            break;
        default:
            break;
        }
    }

    void recv_wait_button(ButtonEvent ev)
    {
        if (ev.button == Button::B || ev.button == Button::Left) enter_chooser();
    }

    void offer_button(ButtonEvent ev)
    {
        switch (ev.button) {
        case Button::A:
        case Button::Start:
            if (!offer_refused_) accept_current_offer();
            break;
        case Button::B:
        case Button::Left:
            enter_chooser();   // decline without immediately re-showing the same offer
            break;
        default:
            break;
        }
    }

    void conflict_button(ButtonEvent ev)
    {
        switch (ev.button) {
        case Button::Up:
            if (conflict_sel_ > 0) {
                conflict_sel_--;
                render_conflict();
            }
            break;
        case Button::Down:
            if (conflict_sel_ < 2) {
                conflict_sel_++;
                render_conflict();
            }
            break;
        case Button::A:
        case Button::Start:
            if (conflict_sel_ == 2) {
                share_bundle_remove_dir(kStagingDir);
                enter_recv_wait();
            } else {
                conflict_decided_ = true;
                conflict_overwrite_ = (conflict_sel_ == 1);
                char slug[33] = {};
                char manifest[200];
                snprintf(manifest, sizeof(manifest), "%s/manifest.cfg", kStagingDir);
                if (!share_bundle_slug_from_manifest(manifest, slug, sizeof(slug))) {
                    share_bundle_remove_dir(kStagingDir);
                    enter_result(false, "App has no usable name", "Tell the sender");
                    return;
                }
                finish_install(slug, app_registry_find_by_slug(slug));
            }
            break;
        default:
            break;
        }
    }

    void result_button(ButtonEvent ev)
    {
        if (ev.button == Button::A || ev.button == Button::Start) enter_chooser();
    }

    Mode mode_ = Mode::Chooser;
    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *detail_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    lv_obj_t *bar_ = nullptr;
    lv_obj_t *pct_ = nullptr;
    lv_obj_t *rows_[5] = {};

    int chooser_sel_ = 0;
    size_t sel_ = 0;
    size_t scroll_ = 0;
    int conflict_sel_ = 0;
    bool conflict_decided_ = false;
    bool conflict_overwrite_ = false;
    bool offer_refused_ = false;

    std::vector<SendItem> send_list_;

    char offer_slug_[SHARE_MAX_SLUG + 1] = {};
    char offer_name_[SHARE_MAX_NAME + 1] = {};
    char offer_ver_[SHARE_MAX_VER + 1] = {};
    size_t offer_len_ = 0;

    std::string result_title_;
    std::string result_hint_;

    int shown_pct_ = -1;
    const char *last_phase_ = nullptr;
    bool wake_held_ = false;
};

static ShareApp s_instance;
App *app_share_instance() { return &s_instance; }
