#include "hal_sleep.h"
#include "hal_display.h"
#include "hal_identity.h"
#include "hal_lights.h"
#include "pins.h"
#include "ui_theme.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#include "lvgl.h"

extern "C" const lv_image_dsc_t sleeping_patch;

static const char *TAG = "hal_sleep";

static constexpr uint32_t kDimAfterMs = 60u * 1000u;
static uint32_t s_idle_timeout_ms = 300u * 1000u;   // 5 minutes

// LVGL renders the 320x240 card through six asynchronous 40-row SPI
// transfers. Keep the MCU awake long enough for every transfer to reach the
// ST7789 before light sleep pauses the peripheral.
static constexpr int64_t kSleepCardSettleUs = 500ll * 1000ll;

// How long a single light-sleep call lasts before we surface to re-evaluate.
// This is also where the ceremonies beacon poll will hook in, which is why
// it is minutes rather than seconds.
static constexpr uint64_t kPollIntervalUs = 180ull * 1000000ull;

static SleepTier s_tier = SLEEP_ACTIVE;
static bool s_wake_lock = false;
static bool s_enter_now = false;
static bool s_inited = false;
static int64_t s_last_activity_us = 0;
static int64_t s_card_shown_us = 0;
static lv_obj_t *s_card = nullptr;

static uint32_t idle_ms()
{
    int64_t now = esp_timer_get_time();
    if (s_last_activity_us == 0) s_last_activity_us = now;
    return (uint32_t)((now - s_last_activity_us) / 1000);
}

esp_err_t hal_sleep_init()
{
    // Start is the one button wired to a bare GPIO rather than the 74HC165
    // shift register, which makes it the only usable wake source. It is
    // active-low with a pull-up, so a LOW level means "pressed".
    gpio_wakeup_enable((gpio_num_t)PIN_BTN_START, GPIO_INTR_LOW_LEVEL);
    esp_err_t err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_gpio_wakeup: %s", esp_err_to_name(err));
        return err;
    }

    s_last_activity_us = esp_timer_get_time();
    s_enter_now = false;
    s_inited = true;
    ESP_LOGI(TAG, "sleep manager ready (idle %lus, wake on START)",
             (unsigned long)(s_idle_timeout_ms / 1000));
    return ESP_OK;
}

void hal_sleep_set_idle_timeout(uint32_t seconds)
{
    if (seconds < 10) seconds = 10;
    s_idle_timeout_ms = seconds * 1000u;
}

void hal_sleep_note_activity()
{
    s_last_activity_us = esp_timer_get_time();
    s_enter_now = false;
    // Any input fully wakes us, including the press that came out of light
    // sleep or one received during the pre-sleep paint-settle window.
    if (s_card) {
        lv_obj_delete(s_card);
        s_card = nullptr;
    }
    s_card_shown_us = 0;
    s_tier = SLEEP_ACTIVE;
}

void hal_sleep_wake_lock(bool on)
{
    s_wake_lock = on;
    if (on) hal_sleep_note_activity();
}

SleepTier hal_sleep_tier()
{
    return s_tier;
}

void hal_sleep_enter_now()
{
    if (!s_inited || s_wake_lock) return;
    s_enter_now = true;
}

// Drawn on the top layer rather than the app root, so the app's UI survives
// underneath and reappears intact on wake — no re-render, no flash of a
// half-built screen.
static void show_card()
{
    if (s_card) return;

    lv_obj_t *top = lv_layer_top();
    s_card = lv_obj_create(top);
    lv_obj_remove_style_all(s_card);
    // Use physical panel dimensions rather than percentages so this overlay
    // always paints every pixel even if a parent layer has stale content size.
    lv_obj_set_size(s_card, 320, 240);
    lv_obj_set_pos(s_card, 0, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(BadgeTheme::BACKGROUND), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    const BadgeIdentity *identity = hal_identity_is_provisioned()
        ? hal_identity_get() : nullptr;

    // Match the My Badge page: role is a full-width lanyard-colour band, not
    // a narrow pill competing with the owner's name for horizontal space.
    uint8_t role_color[3] = { 0x3a, 0x45, 0x60 };
    if (identity) hal_identity_role_default_color(identity->role, role_color);

    lv_obj_t *role = lv_obj_create(s_card);
    lv_obj_remove_style_all(role);
    lv_obj_set_size(role, LV_PCT(100), 26);
    lv_obj_set_style_bg_color(role,
        lv_color_make(role_color[0], role_color[1], role_color[2]), 0);
    lv_obj_set_style_bg_opa(role, LV_OPA_COVER, 0);
    lv_obj_clear_flag(role, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(role, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *role_label = lv_label_create(role);
    lv_label_set_text(role_label,
        identity ? hal_identity_role_name(identity->role) : "Unregistered");
    const uint32_t role_luma = 299u * role_color[0]
                             + 587u * role_color[1]
                             + 114u * role_color[2];
    lv_obj_set_style_text_color(role_label,
        lv_color_hex(role_luma >= 125000u ? BadgeTheme::BACKGROUND : BadgeTheme::TEXT), 0);
    lv_obj_center(role_label);

    // Keep the hero, owner, and resume sections in one fixed-height column.
    // Their two intervening gaps are distributed equally instead of relying
    // on unrelated absolute Y coordinates.
    lv_obj_t *content = lv_obj_create(s_card);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 320, 197);
    lv_obj_set_pos(content, 0, 38);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content,
        LV_FLEX_ALIGN_SPACE_BETWEEN,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hero = lv_obj_create(content);
    lv_obj_remove_style_all(hero);
    lv_obj_set_size(hero, 320, 102);
    lv_obj_clear_flag(hero, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *patch = lv_image_create(hero);
    lv_image_set_src(patch, &sleeping_patch);
    lv_obj_set_pos(patch, 12, 0);

    lv_obj_t *title = lv_label_create(hero);
    lv_label_set_text(title, "Sleeping...");
    lv_obj_set_width(title, 168);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
    lv_obj_set_pos(title, 140, 38);

    lv_obj_t *owner_group = lv_obj_create(content);
    lv_obj_remove_style_all(owner_group);
    lv_obj_set_size(owner_group, 320, 39);
    lv_obj_clear_flag(owner_group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *owner = lv_label_create(owner_group);
    lv_label_set_text(owner, identity ? "THIS BADGE BELONGS TO" : "THIS BADGE IS");
    lv_obj_set_style_text_font(owner, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(owner, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
    lv_obj_align(owner, LV_ALIGN_TOP_MID, 0, 0);

    // A full-width owner line makes a badge left on a table easy to return,
    // while leaving the illustration and sleep state as the visual focus.
    lv_obj_t *who = lv_label_create(owner_group);
    lv_label_set_text(who, identity ? identity->display_name : "Waiting for registration");
    lv_obj_set_width(who, 304);
    lv_label_set_long_mode(who, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(who, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(who, lv_color_hex(BadgeTheme::TEXT), 0);
    lv_obj_align(who, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_t *hint = lv_label_create(content);
    lv_label_set_text(hint, "Start to resume");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);

    s_card_shown_us = esp_timer_get_time();
}

void hal_sleep_tick()
{
#ifdef BADGE_NO_SLEEP
    // Overnight soak builds: never dim, never sleep. Test only.
    (void)s_inited;
    return;
#endif
    if (!s_inited) return;

    if (s_wake_lock) {
        s_last_activity_us = esp_timer_get_time();
        if (s_tier != SLEEP_ACTIVE) hal_sleep_note_activity();
        return;
    }

    uint32_t idle = idle_ms();

    if (s_tier == SLEEP_ACTIVE && (s_enter_now || idle >= kDimAfterMs)) {
        hal_lights_clear();
        hal_lights_show();
        s_tier = SLEEP_DIM;
    }

    if (!s_enter_now && idle < s_idle_timeout_ms) return;

    // Draw the card, then keep normal clocks running for a short settle window.
    // lv_refr_now() queues the final DMA transfer but does not wait for that
    // asynchronous SPI transaction to finish in this double-buffered setup.
    if (s_tier != SLEEP_LIGHT) {
        if (!s_card) {
            show_card();
            lv_refr_now(nullptr);
            return;
        }
        if (esp_timer_get_time() - s_card_shown_us < kSleepCardSettleUs) return;
        s_tier = SLEEP_LIGHT;
    }

    // One sleep per tick rather than an internal loop. We are running inside
    // the LVGL timer callback with the port lock held; returning between
    // sleeps keeps that hold short and lets the rest of the system breathe
    // for a few ms every poll interval.
    esp_sleep_enable_timer_wakeup(kPollIntervalUs);
    esp_light_sleep_start();

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        // Start was pressed. The button task will also queue a real press
        // event; waking here just means the UI is up by the time it lands.
        hal_sleep_note_activity();
        return;
    }

    // Timer wake: nothing to do yet. This is where the ceremonies beacon
    // poll will go — listen briefly with the card still up, and only bring
    // the screen back if a show is live.
}
