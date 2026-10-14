#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "app_scanner.h"
#include "app_sponsors.h"
#include "app_workshops.h"
#include "hal_nfc.h"
#include "launcher_slugs.h"
#include "sponsor_store.h"
#include "workshop_store.h"

#include <cmath>
#include <cstdio>

namespace {

char s_pending_payload[64] = {};

} // namespace

extern "C" const lv_image_dsc_t icon_scanner;
extern "C" const lv_image_dsc_t scanner_sticker;

// Palette matches app_launcher.cpp.
static constexpr uint32_t COLOR_BG       = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_TEXT     = BadgeTheme::TEXT;
static constexpr uint32_t COLOR_DIM      = BadgeTheme::TEXT_MUTED;
static constexpr uint32_t COLOR_ACCENT   = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_GOLD     = 0xffd166;
static constexpr uint32_t COLOR_MAGENTA  = 0xff5c8a;
static constexpr uint32_t COLOR_VIOLET   = 0x8a63ff;

// Tick timer fires at 50 Hz (see app_registry.cpp:86), so 1 tick = 20 ms.
static constexpr int CELEBRATE_TICKS = 125; // 2.5 s
static constexpr int ALREADY_TICKS   = 60;  // 1.2 s
static constexpr int UNKNOWN_TICKS   = 75;  // 1.5 s
static constexpr int ERROR_TICKS     = 50;  // 1.0 s
static constexpr int ZOOM_TICKS      = 20;  // 0.4 s zoom-in
static constexpr int FADE_DELAY      = 10;  // ticks before name label fades in

class ScannerApp : public App {
public:
    const char *name() const override { return "Scanner"; }
    const char *icon() const override { return "SCN"; }
    const char *slug() const override { return "scanner"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_scanner; }
    AppAccess access() const override { return AppAccess::Default; }
    // Entry always reboots first (registry): the tap UX needs a fresh heap
    // and field, and exit reboots back for the same reason.
    bool uses_nfc() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        build_idle_ui();
        hal_nfc_clear_last_card();
        hal_nfc_enable();
        state_ = State::Idle;
        ticks_ = 0;

        if (s_pending_payload[0]) {
            char payload[sizeof(s_pending_payload)];
            snprintf(payload, sizeof(payload), "%s", s_pending_payload);
            s_pending_payload[0] = '\0';
            process_text(payload);
        }
    }

    void on_exit() override
    {
        clear_overlay();
        title_ = status_ = hint_ = nullptr;
        root_ = nullptr;
    }

    void on_tick() override
    {
        switch (state_) {
            case State::Idle: {
                NfcCard card;
                if (!hal_nfc_last_card(&card)) return;
                begin_read();
                break;
            }
            case State::Celebrate:
                animate_celebration();
                if (++ticks_ >= CELEBRATE_TICKS) return_to_idle();
                break;
            case State::Already:
                if (++ticks_ >= ALREADY_TICKS) return_to_idle();
                break;
            case State::Unknown:
                if (++ticks_ >= UNKNOWN_TICKS) return_to_idle();
                break;
            case State::Error:
                if (++ticks_ >= ERROR_TICKS) return_to_idle();
                break;
        }
    }

private:
    enum class State { Idle, Celebrate, Already, Unknown, Error };

    void build_idle_ui()
    {
        clear_overlay();

        title_ = lv_label_create(root_);
        lv_label_set_text(title_, "Scanner");
        lv_obj_set_style_text_color(title_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_24, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t *icon = lv_image_create(root_);
        lv_image_set_src(icon, &scanner_sticker);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 52);

        status_ = lv_label_create(root_);
        lv_label_set_text(status_, "Align your badge's flower pattern\nwith a matching sticker!");
        lv_obj_set_width(status_, 304);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(status_, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_text_font(status_, &lv_font_montserrat_16, 0);
        lv_obj_align(status_, LV_ALIGN_TOP_MID, 0, 160);

        hint_ = lv_label_create(root_);
        lv_label_set_text(hint_, "HOME  apps");
        lv_obj_set_style_text_color(hint_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -12);
    }

    void return_to_idle()
    {
        hal_nfc_clear_last_card();
        build_idle_ui();
        state_ = State::Idle;
        ticks_ = 0;
    }

    void begin_read()
    {
        char text[64] = {};
        esp_err_t err = hal_nfc_read_text(text, sizeof(text));
        // Read done: drop the field before any LED/UI feedback so the flash
        // never overlaps the energized antenna. Latch cleared first so the
        // duty cycler doesn't immediately re-energize; polling resumes itself.
        hal_nfc_clear_last_card();
        hal_nfc_field_down();
        if (err != ESP_OK) {
            show_error("Couldn't read sticker", "Align and try again");
            return;
        }

        process_text(text);
    }

    void process_text(const char *text)
    {
        const SponsorVisitResult visit = sponsor_store_record_payload(text);
        switch (visit.status) {
        case SponsorVisitStatus::NewVisit:
        case SponsorVisitStatus::AlreadyVisited:
            if (visit.status == SponsorVisitStatus::NewVisit) hal_lights_flash_success();
            app_sponsors_prepare_visit(
                visit.table_id,
                visit.status == SponsorVisitStatus::NewVisit);
            app_registry_launch(app_sponsors_instance());
            return;
        case SponsorVisitStatus::InvalidPayload:
            show_error("Invalid sponsor tag", "Check tag format");
            return;
        case SponsorVisitStatus::StorageError:
            show_error("Couldn't save visit", "Try the table again");
            return;
        case SponsorVisitStatus::NotSponsor:
            break;
        }

        const WorkshopVisitResult workshop_visit =
            workshop_store_record_payload(text);
        switch (workshop_visit.status) {
        case WorkshopVisitStatus::NewVisit:
        case WorkshopVisitStatus::AlreadyVisited:
        case WorkshopVisitStatus::QuestComplete:
            if (workshop_visit.status == WorkshopVisitStatus::NewVisit) {
                hal_lights_flash_success();
            }
            app_workshops_prepare_visit(workshop_visit);
            app_registry_launch(app_workshops_instance());
            return;
        case WorkshopVisitStatus::InvalidPayload:
            show_error("Invalid workshop tag", "Check tag format");
            return;
        case WorkshopVisitStatus::StorageError:
            show_error("Couldn't save visit", "Try the workshop again");
            return;
        case WorkshopVisitStatus::NotWorkshop:
            break;
        }

        App *target = app_registry_find_by_slug(text);
        if (!target) {
            show_unknown(text);
            return;
        }
        if (target->access() != AppAccess::Public) {
            show_error("Not a public unlock", "Use an attendee app tag");
            return;
        }

        bool added = launcher_slugs_append(target->slug());
        if (!added) {
            show_already(target);
        } else {
            show_celebration(target);
        }
    }

    void show_celebration(App *target)
    {
        hal_lights_flash_success();
        clear_overlay();
        target_app_ = target;

        big_icon_ = lv_image_create(root_);
        const lv_image_dsc_t *dsc = target->icon_image();
        if (dsc) lv_image_set_src(big_icon_, dsc);
        lv_image_set_scale(big_icon_, 1); // start tiny, lerp up
        lv_obj_align(big_icon_, LV_ALIGN_CENTER, 0, -8);
        lv_image_set_pivot(big_icon_, 24, 24);

        caption_ = lv_label_create(root_);
        lv_label_set_text(caption_, "UNLOCKED");
        lv_obj_set_style_text_color(caption_, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_style_text_font(caption_, &lv_font_montserrat_20, 0);
        lv_obj_align(caption_, LV_ALIGN_TOP_MID, 0, 30);
        lv_obj_set_style_opa(caption_, LV_OPA_TRANSP, 0);

        name_label_ = lv_label_create(root_);
        lv_label_set_text(name_label_, target->name());
        lv_obj_set_style_text_color(name_label_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_24, 0);
        lv_obj_align(name_label_, LV_ALIGN_BOTTOM_MID, 0, -28);
        lv_obj_set_style_opa(name_label_, LV_OPA_TRANSP, 0);

        const uint32_t hues[kOrbitCount] = {
            COLOR_ACCENT, COLOR_MAGENTA, COLOR_GOLD,
            COLOR_VIOLET, COLOR_ACCENT, COLOR_MAGENTA,
        };
        for (int i = 0; i < kOrbitCount; i++) {
            orbits_[i] = lv_obj_create(root_);
            lv_obj_remove_style_all(orbits_[i]);
            lv_obj_set_size(orbits_[i], 10, 10);
            lv_obj_set_style_radius(orbits_[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(orbits_[i], lv_color_hex(hues[i]), 0);
            lv_obj_set_style_bg_opa(orbits_[i], LV_OPA_COVER, 0);
            lv_obj_clear_flag(orbits_[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        state_ = State::Celebrate;
        ticks_ = 0;
    }

    void animate_celebration()
    {
        // Icon zoom: linearly scale 0 -> 512 (2x) over ZOOM_TICKS, then settle.
        if (big_icon_) {
            int t = ticks_ < ZOOM_TICKS ? ticks_ : ZOOM_TICKS;
            int scale = 1 + (512 * t) / ZOOM_TICKS;
            lv_image_set_scale(big_icon_, (uint16_t)scale);
        }
        // Caption + name fade in after a short delay.
        if (ticks_ >= FADE_DELAY) {
            int f = ticks_ - FADE_DELAY;
            int opa = f * 25;
            if (opa > 255) opa = 255;
            if (caption_)    lv_obj_set_style_opa(caption_, (lv_opa_t)opa, 0);
            if (name_label_) lv_obj_set_style_opa(name_label_, (lv_opa_t)opa, 0);
        }
        // Orbit motes on Lissajous curves around the icon center (160, 112).
        float phase = ticks_ * 0.06f;
        for (int i = 0; i < kOrbitCount; i++) {
            if (!orbits_[i]) continue;
            float t = phase + i * (6.2831853f / kOrbitCount);
            float x = 160.0f + 70.0f * sinf(t * 1.1f);
            float y = 112.0f + 56.0f * sinf(t * 0.9f + 1.2f);
            lv_obj_set_pos(orbits_[i], (int)(x - 5), (int)(y - 5));
        }
    }

    void show_already(App *target)
    {
        clear_overlay();
        const lv_image_dsc_t *dsc = target->icon_image();
        if (dsc) {
            big_icon_ = lv_image_create(root_);
            lv_image_set_src(big_icon_, dsc);
            lv_obj_align(big_icon_, LV_ALIGN_CENTER, 0, -16);
        }
        caption_ = lv_label_create(root_);
        lv_label_set_text(caption_, "Already unlocked");
        lv_obj_set_style_text_color(caption_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(caption_, &lv_font_montserrat_20, 0);
        lv_obj_align(caption_, LV_ALIGN_BOTTOM_MID, 0, -36);

        name_label_ = lv_label_create(root_);
        lv_label_set_text(name_label_, target->name());
        lv_obj_set_style_text_color(name_label_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_24, 0);
        lv_obj_align(name_label_, LV_ALIGN_BOTTOM_MID, 0, -12);

        state_ = State::Already;
        ticks_ = 0;
    }

    void show_unknown(const char *slug)
    {
        clear_overlay();
        caption_ = lv_label_create(root_);
        lv_label_set_text(caption_, "Unknown sticker");
        lv_obj_set_style_text_color(caption_, lv_color_hex(COLOR_MAGENTA), 0);
        lv_obj_set_style_text_font(caption_, &lv_font_montserrat_24, 0);
        lv_obj_align(caption_, LV_ALIGN_CENTER, 0, -16);

        name_label_ = lv_label_create(root_);
        lv_label_set_text_fmt(name_label_, "\"%s\"", slug);
        lv_obj_set_style_text_color(name_label_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_18, 0);
        lv_obj_align(name_label_, LV_ALIGN_CENTER, 0, 16);

        state_ = State::Unknown;
        ticks_ = 0;
    }

    void show_error(const char *line1, const char *line2)
    {
        clear_overlay();
        caption_ = lv_label_create(root_);
        lv_label_set_text(caption_, line1);
        lv_obj_set_style_text_color(caption_, lv_color_hex(COLOR_MAGENTA), 0);
        lv_obj_set_style_text_font(caption_, &lv_font_montserrat_20, 0);
        lv_obj_align(caption_, LV_ALIGN_CENTER, 0, -8);

        name_label_ = lv_label_create(root_);
        lv_label_set_text(name_label_, line2);
        lv_obj_set_style_text_color(name_label_, lv_color_hex(COLOR_DIM), 0);
        lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(name_label_, LV_ALIGN_CENTER, 0, 16);

        state_ = State::Error;
        ticks_ = 0;
    }

    void clear_overlay()
    {
        if (root_) lv_obj_clean(root_);
        title_ = status_ = hint_ = nullptr;
        big_icon_ = caption_ = name_label_ = nullptr;
        for (auto &o : orbits_) o = nullptr;
        target_app_ = nullptr;
    }

    static constexpr int kOrbitCount = 6;

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *status_ = nullptr;
    lv_obj_t *hint_ = nullptr;

    lv_obj_t *big_icon_ = nullptr;
    lv_obj_t *caption_ = nullptr;
    lv_obj_t *name_label_ = nullptr;
    lv_obj_t *orbits_[kOrbitCount] = {};

    App *target_app_ = nullptr;
    State state_ = State::Idle;
    int ticks_ = 0;
};

static ScannerApp s_instance;
App *app_scanner_instance() { return &s_instance; }

void app_scanner_prepare_payload(const char *payload)
{
    snprintf(s_pending_payload, sizeof(s_pending_payload), "%s",
             payload ? payload : "");
}
