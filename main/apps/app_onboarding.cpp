#include "app.h"
#include "app_launcher.h"
#include "app_registry.h"
#include "hal_config.h"
#include "hal_gesture.h"
#include "hal_identity.h"
#include "hal_lights.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" const lv_image_dsc_t scanner_sticker;

// First-boot flow: welcome, identity, button tutorial, hardware demos, rules.
//
// Registered as the SHELL rather than a normal app, which is what makes it
// modal for free: app_registry's HOME handler deliberately does not intercept
// HOME while the active app IS the launcher, so the user cannot escape this
// flow until it completes. On finish we swap the real launcher in and open
// the badge ID page.
//
// Completion is recorded as the build number the user finished on, not a
// boolean — a boolean gives no way to force a replay after a change worth
// re-teaching. It lives in LittleFS (not identity), so a factory reset
// legitimately replays the tutorial.

#ifndef BADGE_BUILD_NUMBER
#define BADGE_BUILD_NUMBER 0
#endif

// Sentinel for "never completed". Any value below the compiled-in build
// number replays the tutorial, so this just has to be lower than every real
// build number we will ever ship.
static constexpr int kOnboardUnset = -1;

int badge_onboarding_required()
{
    // Registration owns the first-run gate. An empty badge may run hardware
    // tests, but must never enter or complete the attendee terms/tutorial.
    return hal_identity_is_provisioned()
        && hal_config_get_int("system", "onboard_build", kOnboardUnset) < BADGE_BUILD_NUMBER;
}

void badge_onboarding_reset()
{
    hal_config_set_int("system", "onboard_build", kOnboardUnset);
}

namespace {

struct Step {
    const char *title;
    const char *body;
};

// Kept declarative so copy changes don't touch control flow.
const Step kSteps[] = {
    { nullptr, nullptr },      // personalized welcome
    { nullptr, nullptr },      // identity — rendered from the provisioned record
    { nullptr, nullptr },      // button tutorial — interactive
    { "Lights",
      "Your badge has six LEDs.\n\nApps use them for games,\ntimers, and notifications." },
    { nullptr, nullptr },      // tap tutorial — icon + flashing bottom LEDs
    { nullptr, nullptr },      // shake tutorial — live accelerometer readout
    { nullptr, nullptr },      // Badge Connect — RSVP networking data summary
    { nullptr, nullptr },      // bump tutorial — two badges bumping animation
    { nullptr, nullptr },      // low-battery heat safety warning
    { "Go explore",
      "Games, tools, a scavenger\nhunt, and more to unlock.\n\nBump badges with people\nyou meet to swap contacts." },
    { nullptr, nullptr },      // rules + agreement — interactive
};
constexpr int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr int kStepWelcome  = 0;
constexpr int kStepIdentity = 1;
constexpr int kStepButtons  = 2;
constexpr int kStepLights   = 3;
constexpr int kStepTap      = 4;
constexpr int kStepShake    = 5;
constexpr int kStepConnect  = 6;
constexpr int kStepBump     = 7;
constexpr int kStepHeat     = 8;
constexpr int kStepRules    = kStepCount - 1;

struct ButtonSpec { Button b; const char *label; };
const ButtonSpec kButtonSpecs[] = {
    { Button::Up,    "Up" },    { Button::Down,  "Down" },
    { Button::Left,  "Left" },  { Button::Right, "Right" },
    { Button::Home,  "Home" },  { Button::Start, "Start" },
    { Button::B,     "B" },     { Button::A,     "A" },
};
constexpr int kButtonCount = (int)(sizeof(kButtonSpecs) / sizeof(kButtonSpecs[0]));

// These are logical brightness values: the HAL already applies a 26% physical
// cap and gamma correction. Low values here were rounded to almost no light.
constexpr uint8_t kIdentityLedScale = 192;
constexpr uint8_t kLightsLedScale = 255;
constexpr uint8_t kButtonLedScales[] = { 128, 192, 255 };
constexpr int kBumpFrameTicks = 12;        // ~240 ms per frame at 20 ms/tick
constexpr int kBumpFrameCount = 4;
constexpr int kTapFlashTicks = 17;       // ~340 ms at 20 ms/tick (3x faster)
constexpr uint8_t kTapLedScale = 255;
constexpr uint8_t kShakeLedScale = 255;
static_assert(LED_COUNT == 6, "tap-area LED mapping assumes the six-LED badge");
static_assert(kButtonCount == (int)LED_COUNT + 2,
              "button charge animation expects six LEDs followed by two brightness steps");

} // namespace

class OnboardingApp : public App {
public:
    const char *name() const override { return "Setup"; }
    const char *icon() const override { return "NEW"; }
    const char *slug() const override { return "onboarding"; }
    AppAccess access() const override { return AppAccess::Default; }
    bool wants_wake_lock() const override { return true; }
    bool handles_home_button() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        step_ = 0;
        buttons_done_ = 0;
        agreed_ = false;
        shake_seen_ = false;
        led_phase_ = 0;
        current_force_mg_ = 0;
        peak_force_mg_ = 0;
        shake_force_ = shake_peak_ = shake_hint_ = nullptr;
        bump_left_ = bump_right_ = bump_burst_ = nullptr;
        render();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        root_ = nullptr;
        shake_force_ = shake_peak_ = shake_hint_ = nullptr;
        bump_left_ = bump_right_ = bump_burst_ = nullptr;
    }

    void on_tick() override
    {
        // The lights step animates so "your badge has LEDs" is shown, not
        // just claimed.
        if (step_ == kStepLights) {
            if (++led_phase_ % 6 == 0) {
                update_chase_lights();
            }
        }

        if (step_ == kStepTap) tick_tap_lights();
        if (step_ == kStepShake) tick_shake();
        if (step_ == kStepBump) tick_bump();
    }

    void on_button(ButtonEvent ev) override
    {
        // Onboarding is inert until registration lands.
        if (!hal_identity_is_provisioned()) return;

        if (step_ == kStepButtons) {
            if (ev.kind != ButtonEventKind::Pressed) return;
            // The first B press tests B; subsequent presses navigate back.
            if (ev.button == Button::B && button_done(Button::B)) {
                back();
                return;
            }
            handle_button_tutorial(ev.button);
            return;
        }

        if (ev.kind != ButtonEventKind::Pressed) return;
        if (step_ == kStepWelcome) {
            // Any pushbutton starts the tutorial; Aux1 is the side switch.
            if (ev.button != Button::Aux1) advance();
            return;
        }
        // Back must work even before the shake gate and on the agreement page.
        if (ev.button == Button::B) {
            back();
            return;
        }

        if (step_ == kStepRules) {
            // Require an explicit, separate act of agreement — never let the
            // same press that advanced into this screen also accept it.
            if (ev.button == Button::A) {
                agreed_ = !agreed_;
                render();
            } else if (ev.button == Button::Start && agreed_) {
                finish();
            }
            return;
        }

        if (step_ == kStepShake && !shake_seen_) return;   // must actually shake

        if (ev.button == Button::A || ev.button == Button::Start) advance();
    }

private:
    bool button_done(Button b) const
    {
        for (int i = 0; i < kButtonCount; ++i) {
            if (kButtonSpecs[i].b == b) return (buttons_done_ & (1u << i)) != 0;
        }
        return false;
    }

    void handle_button_tutorial(Button b)
    {
        // Completing the grid only arms the next step. A deliberate extra A
        // press advances, so the eighth physical button is never also the
        // accidental "continue" press.
        if (all_buttons_done()) {
            if (b == Button::A) advance();
            return;
        }

        for (int i = 0; i < kButtonCount; i++) {
            if (kButtonSpecs[i].b != b) continue;
            uint32_t bit = 1u << i;
            if (buttons_done_ & bit) return;
            buttons_done_ |= bit;
            update_button_charge_leds();
            render();
            return;
        }
    }

    bool all_buttons_done() const
    {
        return buttons_done_ == ((1u << kButtonCount) - 1u);
    }

    void set_scaled_led(size_t index, const uint8_t color[3], uint8_t scale)
    {
        hal_lights_set(index,
            (uint8_t)(((uint16_t)color[0] * scale) / 255),
            (uint8_t)(((uint16_t)color[1] * scale) / 255),
            (uint8_t)(((uint16_t)color[2] * scale) / 255));
    }

    void show_identity_lights()
    {
        if (!hal_identity_is_provisioned()) return;
        uint8_t color[3];
        hal_lights_get_role_color(color);
        for (size_t i = 0; i < LED_COUNT; ++i) {
            set_scaled_led(i, color, kIdentityLedScale);
        }
        hal_lights_show();
    }

    int completed_button_count() const
    {
        int count = 0;
        for (int i = 0; i < kButtonCount; i++) {
            if (buttons_done_ & (1u << i)) count++;
        }
        return count;
    }

    void update_button_charge_leds()
    {
        const int count = completed_button_count();
        const size_t lit = count < (int)LED_COUNT ? (size_t)count : LED_COUNT;
        const int brightness_step = count <= (int)LED_COUNT ? 0 : count - (int)LED_COUNT;
        const uint8_t scale = kButtonLedScales[brightness_step];
        uint8_t color[3];
        hal_lights_get_role_color(color);

        for (size_t i = 0; i < LED_COUNT; i++) {
            if (i < lit) set_scaled_led(i, color, scale);
            else         hal_lights_set(i, 0, 0, 0);
        }
        hal_lights_show();
    }

    void update_chase_lights()
    {
        const size_t lit = (led_phase_ / 6) % LED_COUNT;
        uint8_t color[3];
        hal_lights_get_role_color(color);
        for (size_t i = 0; i < LED_COUNT; i++) {
            if (i == lit) set_scaled_led(i, color, kLightsLedScale);
            else          hal_lights_set(i, 0, 0, 0);
        }
        hal_lights_show();
    }

    void show_shake_lights()
    {
        uint8_t color[3];
        hal_lights_get_role_color(color);
        for (size_t i = 0; i < LED_COUNT; i++) {
            set_scaled_led(i, color, kShakeLedScale);
        }
        hal_lights_show();
    }

    void tick_tap_lights()
    {
        // A crisp ~340 ms on/off flash calls attention to the two physical
        // tap-area LEDs without the ambiguity of a soft pulse.
        if (led_phase_ % kTapFlashTicks != 0) {
            led_phase_++;
            return;
        }
        const bool lights_on = ((led_phase_ / kTapFlashTicks) % 2) == 0;
        led_phase_++;

        uint8_t color[3];
        hal_lights_get_role_color(color);

        hal_lights_clear();
        if (lights_on) {
            set_scaled_led(BadgeLed::BottomLeft, color, kTapLedScale);
            set_scaled_led(BadgeLed::BottomRight, color, kTapLedScale);
        }
        hal_lights_show();
    }

    void tick_shake()
    {
        const bool shook = gesture_poll_shake();
        const AccelSample sample = gesture_last_sample();
        current_force_mg_ = (int)lroundf(sqrtf(sample.x_mg * sample.x_mg
                                             + sample.y_mg * sample.y_mg
                                             + sample.z_mg * sample.z_mg));
        if (current_force_mg_ > peak_force_mg_) peak_force_mg_ = current_force_mg_;
        update_shake_readout();

        if (shook && !shake_seen_) {
            shake_seen_ = true;
            show_shake_lights();
            if (shake_hint_) lv_label_set_text(shake_hint_, "A / START: next   B: back");
        }
    }

    void update_shake_readout()
    {
        char buf[32];
        if (shake_force_) {
            snprintf(buf, sizeof(buf), "G-force  %d mg", current_force_mg_);
            lv_label_set_text(shake_force_, buf);
        }
        if (shake_peak_) {
            snprintf(buf, sizeof(buf), "Peak     %d mg", peak_force_mg_);
            lv_label_set_text(shake_peak_, buf);
        }
    }

    void advance()
    {
        if (step_ + 1 >= kStepCount) return;
        int next = step_ + 1;
        if (next == kStepButtons && all_buttons_done()) ++next;
        go_to_step(next);
    }

    void back()
    {
        if (step_ == kStepWelcome) return;
        int previous = step_ - 1;
        // Going back from Lights skips the completed hardware test.
        if (previous == kStepButtons) --previous;
        agreed_ = false;
        go_to_step(previous);
    }

    void go_to_step(int step)
    {
        step_ = step;
        led_phase_ = 0;
        hal_lights_clear();
        hal_lights_show();
        render();
        if (step_ == kStepButtons) update_button_charge_leds();
    }

    void finish()
    {
        if (!hal_identity_is_provisioned()) return;
        hal_config_set_int("system", "onboard_build", BADGE_BUILD_NUMBER);
        hal_lights_clear();
        hal_lights_show();
        // Hand the shell back to the real launcher, then open the ID page —
        // the same place a normal boot lands.
        app_registry_set_launcher(app_launcher_instance());
        app_registry_launch(app_badge_id_instance());
    }

    void render()
    {
        if (!root_) return;
        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        shake_force_ = shake_peak_ = shake_hint_ = nullptr;
        bump_left_ = bump_right_ = bump_burst_ = nullptr;

        if (step_ == kStepWelcome)  { render_welcome();  return; }
        if (step_ == kStepIdentity) { render_identity(); return; }
        if (step_ == kStepButtons)  { render_buttons();  return; }
        if (step_ == kStepTap)      { render_tap();      return; }
        if (step_ == kStepShake)    { render_shake();    return; }
        if (step_ == kStepConnect)  { render_connect();  return; }
        if (step_ == kStepBump)     { render_bump();     return; }
        if (step_ == kStepHeat)     { render_heat_safety(); return; }
        if (step_ == kStepRules)    { render_rules();    return; }

        // Show the first frame immediately when entering or revisiting Lights.
        if (step_ == kStepLights) update_chase_lights();
        const Step &s = kSteps[step_];
        title(s.title);
        body(s.body);
        footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
        progress();
    }

    void render_welcome()
    {
        hal_lights_clear();
        hal_lights_show();
        const BadgeIdentity *id = hal_identity_is_provisioned()
            ? hal_identity_get() : nullptr;
        // `display_name` is the persisted name of the registration site's
        // “Badge name” field. Treat it as the attendee's canonical Badge name,
        // not as a generic UI nickname or a hard-coded sample value.
        const char *badge_name =
            (id && id->display_name[0]) ? id->display_name : "Friend";

        lv_obj_t *greeting = lv_label_create(root_);
        lv_label_set_text(greeting, "Welcome to\nHack the North");
        lv_obj_set_width(greeting, 224);
        lv_obj_set_style_text_font(greeting, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(greeting, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(greeting, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(greeting, LV_ALIGN_TOP_MID, 0, 5);

        // Reserve room for two lines so long Badge names never collapse the
        // deliberate vertical rhythm or run into the ownership copy.
        lv_obj_t *name = lv_label_create(root_);
        lv_label_set_text(name, badge_name);
        lv_obj_set_width(name, 296);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 64);

        lv_obj_t *badge_copy = lv_label_create(root_);
        lv_label_set_text(badge_copy, "This is your badge.");
        lv_obj_set_style_text_font(badge_copy, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(badge_copy, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(badge_copy, LV_ALIGN_TOP_MID, 0, 128);

        lv_obj_t *ownership = lv_label_create(root_);
        lv_label_set_text(ownership,
            "It is yours to keep throughout\n"
            "and after the event.");
        lv_obj_set_width(ownership, 304);
        lv_obj_set_style_text_font(ownership, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(ownership, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(ownership, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(ownership, LV_ALIGN_TOP_MID, 0, 164);

        footer("Any button to start");
        progress();
    }

    void render_identity()
    {
        if (!hal_identity_is_provisioned()) {
            title("Not registered");
            body("This badge isn't linked to\nan attendee yet.\n\n"
                 "Bring it to a registration\ndesk to get set up.");
            footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
            return;
        }
        const BadgeIdentity *id = hal_identity_get();
        title("This is you");

        lv_obj_t *nm = lv_label_create(root_);
        // The registration site's Badge name is the canonical attendee name
        // everywhere in onboarding.
        lv_label_set_text(nm, id->display_name);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(nm, LV_ALIGN_CENTER, 0, -10);

        lv_obj_t *rl = lv_label_create(root_);
        lv_label_set_text(rl, hal_identity_role_name(id->role));
        lv_obj_set_style_text_color(rl,
            lv_color_make(id->role_color[0], id->role_color[1], id->role_color[2]), 0);
        lv_obj_align(rl, LV_ALIGN_CENTER, 0, 20);

        // Role colour on the LEDs at the moment they see their own name.
        show_identity_lights();

        footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
        progress();
    }

    void render_buttons()
    {
        title("Try every button");
        // Mirror the momentary controls: D-pad on the left, B/A on the right,
        // and Home/Start beneath them. The infrequently used maintained side
        // switch is intentionally left out of the attendee tutorial.
        make_button_pad(Button::Up,   82,  82, 48, 30, "UP");
        make_button_pad(Button::Left, 46, 118, 48, 30, "LEFT");
        make_button_pad(Button::Right,118,118, 48, 30, "RIGHT");
        make_button_pad(Button::Down, 82, 154, 48, 30, "DOWN");
        make_button_pad(Button::B,   224,  92, 44, 34, "B");
        make_button_pad(Button::A,   280,  92, 44, 34, "A");
        make_button_pad(Button::Home,216, 150, 60, 32, "Home");
        make_button_pad(Button::Start,280,150, 60, 32, "Start");

        footer(all_buttons_done() ? "A: next   B: back"
               : button_done(Button::B) ? "Try every button   B: back"
                                        : "Press all 8 buttons once");
        progress();
    }

    void make_button_pad(Button b, int cx, int cy, int w, int h, const char *label)
    {
        bool done = false;
        for (int i = 0; i < kButtonCount; i++) {
            if (kButtonSpecs[i].b == b) {
                done = (buttons_done_ & (1u << i)) != 0;
                break;
            }
        }

        lv_obj_t *pad = lv_obj_create(root_);
        lv_obj_remove_style_all(pad);
        lv_obj_set_size(pad, w, h);
        lv_obj_set_pos(pad, cx - w / 2, cy - h / 2);
        lv_obj_set_style_radius(pad, 6, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(pad, lv_color_hex(done ? 0x36e07a : 0x111a2a), 0);
        lv_obj_set_style_border_width(pad, 2, 0);
        lv_obj_set_style_border_color(pad, lv_color_hex(done ? 0x36e07a : 0x3a526f), 0);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *text = lv_label_create(pad);
        lv_label_set_text(text, label);
        lv_obj_set_style_text_font(text, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(text, lv_color_hex(done ? 0x04120a : BadgeTheme::TEXT), 0);
        lv_obj_center(text);
    }

    void render_tap()
    {
        title("Tap to unlock");

        lv_obj_t *top = lv_label_create(root_);
        lv_label_set_text(top, "OPEN Scanner to scan tags.\n"
                               "Look for this sticker at the event.");
        lv_obj_set_width(top, 304);
        lv_obj_set_style_text_font(top, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_align(top, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(top, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 42);

        lv_obj_t *image = lv_image_create(root_);
        lv_image_set_src(image, &scanner_sticker);
        lv_obj_align(image, LV_ALIGN_CENTER, 0, 8);

        lv_obj_t *bottom = lv_label_create(root_);
        lv_label_set_text(bottom, "Align the badge with the sticker.");
        lv_obj_set_style_text_color(bottom, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(bottom, LV_ALIGN_CENTER, 0, 77);

        tick_tap_lights();
        footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
        progress();
    }

    void render_shake()
    {
        title("Shake it!");

        lv_obj_t *instruction = lv_label_create(root_);
        lv_label_set_text(instruction, "Give the badge a shake.");
        lv_obj_set_style_text_color(instruction, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(instruction, LV_ALIGN_TOP_MID, 0, 54);

        shake_force_ = lv_label_create(root_);
        lv_obj_set_style_text_font(shake_force_, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(shake_force_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(shake_force_, LV_ALIGN_CENTER, 0, -6);

        shake_peak_ = lv_label_create(root_);
        lv_obj_set_style_text_color(shake_peak_, lv_color_hex(0x36e07a), 0);
        lv_obj_align(shake_peak_, LV_ALIGN_CENTER, 0, 26);

        shake_hint_ = lv_label_create(root_);
        lv_label_set_text(shake_hint_, shake_seen_ ? "A / START: next   B: back"
                                                   : "Shake to continue   B: back");
        lv_obj_set_style_text_color(shake_hint_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(shake_hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        update_shake_readout();
        if (shake_seen_) show_shake_lights();
        progress();
    }

    void render_connect()
    {
        title("Badge Connect");

        const BadgeIdentity *id = hal_identity_get();

        lv_obj_t *intro = lv_label_create(root_);
        lv_label_set_text(intro, "During RSVP, you chose to share:");
        lv_obj_set_style_text_font(intro, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(intro, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(intro, LV_ALIGN_TOP_MID, 0, 38);

        struct SharedField { const char *label; const char *value; };
        const SharedField fields[] = {
            { "Badge name", id->display_name },
            { "Email",      id->net_email },
            { "Phone",      id->net_phone },
            { "LinkedIn",   id->net_linkedin },
            { "Discord",    id->net_discord },
            { "Instagram",  id->net_instagram },
            { "Twitter/X",  id->net_x },
        };

        int row = 0;
        for (const SharedField &field : fields) {
            // Keep every row in a stable position. Omitting empty RSVP values
            // made the layout depend on a person's data and let longer labels
            // such as Instagram reflow differently from the other rows.
            const bool is_set = field.value[0] != '\0';
            const int y = 56 + row * 16;

            lv_obj_t *label = lv_label_create(root_);
            lv_label_set_text_fmt(label, "%s:", field.label);
            lv_obj_set_size(label, 96, 16);
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            lv_obj_set_pos(label, 10, y);

            lv_obj_t *value = lv_label_create(root_);
            lv_label_set_text(value, is_set ? field.value : "Not set");
            lv_obj_set_size(value, 202, 16);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_font(value, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(value,
                lv_color_hex(is_set ? BadgeTheme::TEXT : 0x5c6a84), 0);
            lv_obj_set_pos(value, 112, y);

            row++;
        }

        lv_obj_t *notice = lv_label_create(root_);
        lv_label_set_text(notice,
            "Shared with attendees you bump.\n"
            "Edit at my.hackthenorth.com/badge");
        lv_obj_set_width(notice, 310);
        lv_obj_set_style_text_font(notice, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(notice, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(notice, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(notice, LV_ALIGN_TOP_MID, 0, 174);

        footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
        progress();
    }

    // Bump tutorial: two badge slabs slide side to side and tap together.
    // A deliberately tiny frame table — apart, closing, touching, closing —
    // so the motion reads as a bump without any lv_anim dependency.
    lv_obj_t *make_bump_badge(int w, int h, uint32_t border)
    {
        lv_obj_t *badge = lv_obj_create(root_);
        lv_obj_remove_style_all(badge);
        lv_obj_set_size(badge, w, h);
        lv_obj_set_style_bg_color(badge, lv_color_hex(BadgeTheme::PANEL), 0);
        lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(badge, 2, 0);
        lv_obj_set_style_border_color(badge, lv_color_hex(border), 0);
        lv_obj_set_style_radius(badge, 8, 0);
        lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *screen = lv_obj_create(badge);
        lv_obj_remove_style_all(screen);
        lv_obj_set_size(screen, w - 24, h - 44);
        lv_obj_align(screen, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_bg_color(screen, lv_color_hex(BadgeTheme::SURFACE), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(screen, 1, 0);
        lv_obj_set_style_border_color(screen, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_radius(screen, 3, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *led = lv_obj_create(badge);
        lv_obj_remove_style_all(led);
        lv_obj_set_size(led, w - 24, 8);
        lv_obj_align(led, LV_ALIGN_BOTTOM_MID, 0, -8);
        lv_obj_set_style_bg_color(led, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(led, 4, 0);
        lv_obj_clear_flag(led, LV_OBJ_FLAG_SCROLLABLE);
        return badge;
    }

    void update_bump_frame()
    {
        if (!bump_left_ || !bump_right_) return;
        // Gap from screen center to each badge edge per frame: apart,
        // closing, touching (bump!), closing. Four frames, looping.
        static constexpr int kGaps[kBumpFrameCount] = { 30, 13, 2, 13 };
        const int frame = (led_phase_ / kBumpFrameTicks) % kBumpFrameCount;
        static constexpr int kBadgeW = 64;
        static constexpr int kBadgeY = 46;
        const int gap = kGaps[frame];
        lv_obj_set_pos(bump_left_, 160 - gap - kBadgeW, kBadgeY);
        lv_obj_set_pos(bump_right_, 160 + gap, kBadgeY);
        if (bump_burst_) {
            if (frame == 2) lv_obj_clear_flag(bump_burst_, LV_OBJ_FLAG_HIDDEN);
            else            lv_obj_add_flag(bump_burst_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    void tick_bump()
    {
        if (!bump_left_ || !bump_right_) return;
        led_phase_++;
        if (led_phase_ % kBumpFrameTicks != 0) return;
        update_bump_frame();
    }

    void render_bump()
    {
        title("Bump to connect");

        static constexpr int kBadgeW = 64;
        static constexpr int kBadgeH = 84;
        bump_left_ = make_bump_badge(kBadgeW, kBadgeH, BadgeTheme::ACCENT_DETAIL);
        bump_right_ = make_bump_badge(kBadgeW, kBadgeH, BadgeTheme::TEXT_SOFT);

        // Impact flash at the contact point, on top of the touching badges.
        bump_burst_ = lv_label_create(root_);
        lv_label_set_text(bump_burst_, "BUMP!");
        lv_obj_set_style_text_font(bump_burst_, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(bump_burst_, lv_color_hex(0x36e07a), 0);
        lv_obj_align(bump_burst_, LV_ALIGN_CENTER, 0, -30);
        lv_obj_add_flag(bump_burst_, LV_OBJ_FLAG_HIDDEN);

        update_bump_frame();

        lv_obj_t *copy = lv_label_create(root_);
        lv_label_set_text(copy, "Bump together gently");
        lv_obj_set_width(copy, 310);
        lv_obj_set_style_text_font(copy, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(copy, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(copy, LV_ALIGN_TOP_MID, 0, 146);

        lv_obj_t *note = lv_label_create(root_);
        lv_label_set_text(note, "Only works while Connect app is open");
        lv_obj_set_width(note, 310);
        lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(note, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(note, LV_ALIGN_TOP_MID, 0, 180);

        footer("A / START: next   B: back");
        progress();
    }

    void render_heat_safety()
    {
        // This warning intentionally breaks from the usual violet onboarding
        // palette. It needs to read as safety guidance, not another feature.
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x160406), 0);

        lv_obj_t *heading = lv_label_create(root_);
        lv_label_set_text(heading, "HEAT SAFETY");
        lv_obj_set_style_text_font(heading, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(heading, lv_color_hex(0xff5a5f), 0);
        lv_obj_set_pos(heading, 10, 6);

        lv_obj_t *card = lv_obj_create(root_);
        lv_obj_remove_style_all(card);
        lv_obj_set_pos(card, 8, 38);
        lv_obj_set_size(card, 304, 168);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x2a090c), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xd93f45), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 9, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *alert = lv_obj_create(card);
        lv_obj_remove_style_all(alert);
        lv_obj_set_pos(alert, 14, 12);
        lv_obj_set_size(alert, 38, 38);
        lv_obj_set_style_bg_color(alert, lv_color_hex(0xe5484d), 0);
        lv_obj_set_style_bg_opa(alert, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(alert, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *mark = lv_label_create(alert);
        lv_label_set_text(mark, "!");
        lv_obj_set_style_text_font(mark, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(mark, lv_color_hex(0xffffff), 0);
        lv_obj_center(mark);

        lv_obj_t *warning = lv_label_create(card);
        lv_label_set_text(warning,
            "Low batteries can make parts of\n"
            "the badge, especially the TOP,\n"
            "extremely hot.");
        lv_obj_set_width(warning, 232);
        lv_obj_set_style_text_font(warning, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(warning, lv_color_hex(0xffd4d5), 0);
        lv_obj_set_pos(warning, 62, 7);

        lv_obj_t *divider = lv_obj_create(card);
        lv_obj_remove_style_all(divider);
        lv_obj_set_pos(divider, 12, 61);
        lv_obj_set_size(divider, 280, 1);
        lv_obj_set_style_bg_color(divider, lv_color_hex(0x8d292e), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

        lv_obj_t *symptoms = lv_label_create(card);
        lv_label_set_text(symptoms,
            "If the screen glitches or fuzzes,\n"
            "or the top feels warm or hot:");
        lv_obj_set_width(symptoms, 280);
        lv_obj_set_style_text_font(symptoms, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(symptoms, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(symptoms, lv_color_hex(0xf2b7ba), 0);
        lv_obj_set_pos(symptoms, 12, 70);

        lv_obj_t *action = lv_obj_create(card);
        lv_obj_remove_style_all(action);
        lv_obj_set_pos(action, 12, 108);
        lv_obj_set_size(action, 280, 52);
        lv_obj_set_style_bg_color(action, lv_color_hex(0x941b20), 0);
        lv_obj_set_style_bg_opa(action, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(action, 6, 0);

        lv_obj_t *action_text = lv_label_create(action);
        lv_label_set_text(action_text,
            "TURN IT OFF IMMEDIATELY\n"
            "Do not touch the top.\n"
            "Bring it to the Badge Help Desk.");
        lv_obj_set_width(action_text, 268);
        lv_obj_set_style_text_font(action_text, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(action_text, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(action_text, lv_color_hex(0xffffff), 0);
        lv_obj_center(action_text);

        footer(step_ == kStepWelcome ? "A / START: next" : "A / START: next   B: back");
        progress();
    }

    void render_rules()
    {
        title("Badge rules");

        lv_obj_t *copy = lv_label_create(root_);
        lv_label_set_text(copy,
            "Make sure to follow the rules written on the back of your badge at all times.\n\n"
            "By using this badge, I agree to follow the full rules given at:");
        lv_obj_set_width(copy, 296);
        lv_label_set_long_mode(copy, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(copy, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(copy, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(copy, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(copy, LV_ALIGN_TOP_MID, 0, 48);

        lv_obj_t *url = lv_label_create(root_);
        lv_label_set_text(url, "badge.hackthenorth.com");
        lv_obj_set_width(url, 304);
        lv_label_set_long_mode(url, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(url, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(url, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 158);

        lv_obj_t *back_hint = lv_label_create(root_);
        lv_label_set_text(back_hint, "B: back");
        lv_obj_set_style_text_font(back_hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(back_hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(back_hint, LV_ALIGN_TOP_MID, 0, 183);

        lv_obj_t *row = lv_obj_create(root_);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 296, 30);
        lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -13);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *box = lv_obj_create(row);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 22, 22);
        lv_obj_align(box, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_radius(box, 4, 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(agreed_ ? 0x36e07a : 0x111a2a), 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(agreed_ ? 0x36e07a : 0x3a526f), 0);

        if (agreed_) {
            lv_obj_t *mark = lv_label_create(box);
            lv_label_set_text(mark, LV_SYMBOL_OK);
            lv_obj_set_style_text_color(mark, lv_color_hex(0x04120a), 0);
            lv_obj_center(mark);
        }

        lv_obj_t *agree = lv_label_create(row);
        lv_label_set_text(agree, "I agree");
        lv_obj_set_style_text_color(agree, lv_color_hex(agreed_ ? 0x36e07a : BadgeTheme::TEXT), 0);
        lv_obj_align(agree, LV_ALIGN_LEFT_MID, 32, 0);

        lv_obj_t *action = lv_label_create(row);
        lv_label_set_text(action, agreed_ ? "Finished setup   Start" : "Press A to accept.");
        lv_obj_set_style_text_font(action, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(action, lv_color_hex(agreed_ ? 0x36e07a : BadgeTheme::TEXT), 0);
        lv_obj_align(action, LV_ALIGN_RIGHT_MID, 0, 0);

        progress();
    }

    void title(const char *t)
    {
        if (!t) return;
        lv_obj_t *lb = lv_label_create(root_);
        lv_label_set_text(lb, t);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(lb, LV_ALIGN_TOP_MID, 0, 8);
    }

    void body(const char *t)
    {
        if (!t) return;
        lv_obj_t *lb = lv_label_create(root_);
        lv_label_set_text(lb, t);
        lv_obj_set_style_text_align(lb, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(lb, LV_ALIGN_CENTER, 0, 0);
    }

    void footer(const char *t)
    {
        lv_obj_t *lb = lv_label_create(root_);
        lv_label_set_text(lb, t);
        lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    void progress()
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d/%d", step_ + 1, kStepCount);
        lv_obj_t *lb = lv_label_create(root_);
        lv_label_set_text(lb, buf);
        lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(lb, LV_ALIGN_TOP_RIGHT, -6, 8);
    }

    lv_obj_t *root_ = nullptr;
    int step_ = 0;
    uint32_t buttons_done_ = 0;
    bool agreed_ = false;
    bool shake_seen_ = false;
    int led_phase_ = 0;
    int current_force_mg_ = 0;
    int peak_force_mg_ = 0;
    lv_obj_t *shake_force_ = nullptr;
    lv_obj_t *shake_peak_ = nullptr;
    lv_obj_t *shake_hint_ = nullptr;
    lv_obj_t *bump_left_ = nullptr;
    lv_obj_t *bump_right_ = nullptr;
    lv_obj_t *bump_burst_ = nullptr;
};

static OnboardingApp s_instance;
App *app_onboarding_instance() { return &s_instance; }
