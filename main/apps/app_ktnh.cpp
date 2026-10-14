#include "app.h"

#include "esp_random.h"

#include <cstdint>

namespace {

constexpr const char *kManualUrl = "https://badge.hackthenorth.com/ktnh";
constexpr uint32_t kWrongPenaltyMs = 10000;
constexpr uint32_t kFeedbackMs = 1000;

constexpr uint32_t kRed = 0xff5364;
constexpr uint32_t kBlue = 0x45a3ff;
constexpr uint32_t kGreen = 0x43d18d;
constexpr uint32_t kGold = 0xf0bd45;

const char *const kColourNames[] = { "RED", "BLUE", "GREEN", "GOLD" };
const uint32_t kColours[] = { kRed, kBlue, kGreen, kGold };
const char *const kDirections[] = { "U", "R", "D", "L" };

// Every glyph appears in two rows. Row pairs share only two glyphs, in
// opposite orders, so each four-glyph puzzle still identifies exactly one row.
const char *const kGlyphColumns[4][6] = {
    { "@", "#", "?", "&", "%", "+" },
    { "!", "?", "$", "@", "=", "*" },
    { "&", "~", "=", "#", "!", "^" },
    { "^", "+", "*", "%", "$", "~" },
};

const uint8_t kDialAnswers[4][4] = {
    { 7, 3, 9, 1 },
    { 4, 8, 2, 6 },
    { 0, 5, 1, 9 },
    { 6, 2, 8, 4 },
};

const char *const kLockWords[] = {
    "GOOSE", "MAPLE", "WATER", "NIGHT",
    "BUILD", "NORTH", "LASER", "CLOUD",
};

const uint16_t kLockAnswers[8][2] = {
    { 314, 271 }, { 502, 846 }, { 731,  94 }, { 268, 513 },
    { 905, 427 }, { 146, 680 }, { 893, 352 }, { 470, 925 },
};

enum class Screen : uint8_t { Briefing, Lobby, Playing, Won, Lost };
enum class Module : uint8_t {
    PatchCables,
    GlyphStack,
    FrequencyDial,
    ArrowRelay,
    WordLock,
    SimonHonks,
};

extern "C" const lv_image_dsc_t icon_ktnh;

class KtnhApp final : public GameApp {
public:
    const char *name() const override { return "Keep Talking and Nobody Honks"; }
    const char *icon() const override { return "HNK"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_ktnh; }
    const char *slug() const override { return "ktnh"; }
    bool wants_wake_lock() const override { return screen_ == Screen::Playing; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        screen_ = Screen::Briefing;
        module_count_ = 3;
        rng_state_ = esp_random() ^ lv_tick_get() ^ 0x484f4e4bu;
        if (rng_state_ == 0) rng_state_ = 0x6d2b79f5u;
        render();
    }

    void on_exit() override
    {
        root_ = nullptr;
        timer_label_ = nullptr;
        feedback_label_ = nullptr;
        simon_light_ = nullptr;
        simon_colour_label_ = nullptr;
    }

    void on_button(ButtonEvent event) override
    {
        if (event.kind != ButtonEventKind::Pressed) return;

        switch (screen_) {
            case Screen::Briefing:
                if (event.button == Button::A || event.button == Button::Start) {
                    screen_ = Screen::Lobby;
                    render();
                }
                break;
            case Screen::Lobby:
                handle_lobby_button(event.button);
                break;
            case Screen::Playing:
                handle_module_button(event.button);
                break;
            case Screen::Won:
            case Screen::Lost:
                if (event.button == Button::Start || event.button == Button::A) {
                    start_game();
                } else if (event.button == Button::B) {
                    screen_ = Screen::Lobby;
                    render();
                }
                break;
        }
    }

    void on_home_confirmation_cancel(uint32_t paused_ms) override
    {
        last_tick_ += paused_ms;
        simon_started_at_ += paused_ms;
    }

    void on_tick() override
    {
        if (screen_ != Screen::Playing) return;

        const uint32_t now = lv_tick_get();
        const uint32_t delta = now - last_tick_;
        last_tick_ = now;

        if (delta >= remaining_ms_) {
            remaining_ms_ = 0;
            screen_ = Screen::Lost;
            render();
            return;
        }
        remaining_ms_ -= delta;

        const uint32_t shown_seconds = (remaining_ms_ + 999) / 1000;
        if (shown_seconds != last_shown_seconds_) {
            last_shown_seconds_ = shown_seconds;
            update_timer();
        }

        if (feedback_remaining_ms_ > 0) {
            if (delta >= feedback_remaining_ms_) {
                feedback_remaining_ms_ = 0;
                update_feedback();
            } else {
                feedback_remaining_ms_ -= delta;
            }
        }

        if (current_module() == Module::SimonHonks) update_simon(now);
    }

private:
    uint32_t random_u32()
    {
        uint32_t value = rng_state_;
        value ^= value << 13;
        value ^= value >> 17;
        value ^= value << 5;
        rng_state_ = value;
        return value;
    }

    int random_index(int count)
    {
        return static_cast<int>(random_u32() % static_cast<uint32_t>(count));
    }

    lv_obj_t *make_label(const char *text, const lv_font_t *font,
                         uint32_t colour = BadgeTheme::TEXT)
    {
        lv_obj_t *label = lv_label_create(root_);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
        return label;
    }

    lv_obj_t *make_box(int x, int y, int width, int height,
                       uint32_t fill = BadgeTheme::PANEL)
    {
        lv_obj_t *box = lv_obj_create(root_);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_size(box, width, height);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_set_style_radius(box, 6, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(BadgeTheme::BORDER), 0);
        return box;
    }

    void reset_root()
    {
        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        timer_label_ = nullptr;
        feedback_label_ = nullptr;
        simon_light_ = nullptr;
        simon_colour_label_ = nullptr;
    }

    void render()
    {
        if (!root_) return;
        switch (screen_) {
            case Screen::Briefing: render_briefing(); break;
            case Screen::Lobby:    render_lobby(); break;
            case Screen::Playing:  render_playing(); break;
            case Screen::Won:
            case Screen::Lost:     render_result(); break;
        }
    }

    void render_briefing()
    {
        reset_root();

        lv_obj_t *kicker = make_label("GROUP GAME: 2+ PEOPLE", &lv_font_montserrat_14,
                                      BadgeTheme::ACCENT_DETAIL);
        lv_obj_align(kicker, LV_ALIGN_TOP_MID, 0, 7);

        lv_obj_t *title = make_label("KEEP TALKING\nAND NOBODY HONKS", &lv_font_montserrat_20);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 29);

        lv_obj_t *operator_box = make_box(12, 86, 142, 90);
        lv_obj_t *operator_title = lv_label_create(operator_box);
        lv_label_set_text(operator_title, "1  OPERATOR");
        lv_obj_set_style_text_font(operator_title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(operator_title, lv_color_hex(kGold), 0);
        lv_obj_align(operator_title, LV_ALIGN_TOP_LEFT, 8, 7);
        lv_obj_t *operator_copy = lv_label_create(operator_box);
        lv_label_set_text(operator_copy, "Hold the badge.\nDescribe every puzzle.");
        lv_obj_set_width(operator_copy, 124);
        lv_label_set_long_mode(operator_copy, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(operator_copy, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(operator_copy, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(operator_copy, LV_ALIGN_TOP_LEFT, 8, 31);

        lv_obj_t *expert_box = make_box(166, 86, 142, 90);
        lv_obj_t *expert_title = lv_label_create(expert_box);
        lv_label_set_text(expert_title, "1+ EXPERTS");
        lv_obj_set_style_text_font(expert_title, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(expert_title, lv_color_hex(kBlue), 0);
        lv_obj_align(expert_title, LV_ALIGN_TOP_LEFT, 8, 7);
        lv_obj_t *expert_copy = lv_label_create(expert_box);
        lv_label_set_text(expert_copy, "Read the manual.\nHelp with\nNO PEEKING.");
        lv_obj_set_width(expert_copy, 124);
        lv_label_set_long_mode(expert_copy, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(expert_copy, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(expert_copy, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(expert_copy, LV_ALIGN_TOP_LEFT, 8, 31);

        lv_obj_t *rule = make_label("Talk fast. Wrong answers cost 10 seconds.",
                                    &lv_font_montserrat_14, BadgeTheme::TEXT_MUTED);
        lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 185);
        lv_obj_t *hint = make_label("A: OPEN READY ROOM", &lv_font_montserrat_14,
                                    BadgeTheme::ACCENT_DETAIL);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    void render_lobby()
    {
        reset_root();

        lv_obj_t *title = make_label("READY ROOM", &lv_font_montserrat_20);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        lv_obj_t *qr = lv_qrcode_create(root_);
        lv_qrcode_set_size(qr, 128);
        lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(qr, lv_color_hex(0xffffff));
        lv_qrcode_set_data(qr, kManualUrl);
        lv_obj_set_pos(qr, 12, 35);

        lv_obj_t *experts = make_label("EXPERTS", &lv_font_montserrat_14, kBlue);
        lv_obj_set_pos(experts, 157, 43);
        lv_obj_t *scan = make_label("Read the manual.\nHelp with\nNO PEEKING.",
                                    &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        lv_obj_set_width(scan, 151);
        lv_label_set_long_mode(scan, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(scan, 157, 65);

        lv_obj_t *difficulty = make_label("MODULES", &lv_font_montserrat_14, kGold);
        lv_obj_set_pos(difficulty, 157, 122);
        lv_obj_t *count = make_label("", &lv_font_montserrat_24);
        lv_label_set_text_fmt(count, "%c  %d  %c", module_count_ > 1 ? '<' : ' ',
                              module_count_, module_count_ < 6 ? '>' : ' ');
        lv_obj_set_pos(count, 157, 141);

        lv_obj_t *url = make_label(kManualUrl, &lv_font_montserrat_14,
                                   BadgeTheme::ACCENT_DETAIL);
        lv_obj_set_width(url, 304);
        lv_label_set_long_mode(url, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(url, LV_ALIGN_TOP_MID, 0, 171);

        lv_obj_t *time = make_label("", &lv_font_montserrat_14, BadgeTheme::TEXT_MUTED);
        const int total_seconds = 60 + 30 * module_count_;
        lv_label_set_text_fmt(time, "L/R: difficulty   time %d:%02d",
                              total_seconds / 60, total_seconds % 60);
        lv_obj_align(time, LV_ALIGN_TOP_MID, 0, 195);

        lv_obj_t *hint = make_label("START: EVERYBODY READY   B: BACK",
                                    &lv_font_montserrat_14, BadgeTheme::TEXT);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);
    }

    void handle_lobby_button(Button button)
    {
        if (button == Button::Left && module_count_ > 1) {
            module_count_--;
            render_lobby();
        } else if (button == Button::Right && module_count_ < 6) {
            module_count_++;
            render_lobby();
        } else if (button == Button::Start || button == Button::A) {
            start_game();
        } else if (button == Button::B) {
            screen_ = Screen::Briefing;
            render_briefing();
        }
    }

    void start_game()
    {
        unit_code_ = 10 + random_index(90);
        Module all_modules[] = {
            Module::PatchCables,
            Module::GlyphStack,
            Module::FrequencyDial,
            Module::ArrowRelay,
            Module::WordLock,
            Module::SimonHonks,
        };
        for (int i = 5; i > 0; --i) {
            const int j = random_index(i + 1);
            const Module swap = all_modules[i];
            all_modules[i] = all_modules[j];
            all_modules[j] = swap;
        }
        for (int i = 0; i < module_count_; ++i) modules_[i] = all_modules[i];

        module_index_ = 0;
        solved_count_ = 0;
        module_picker_open_ = false;
        for (auto &state : module_states_) state = {};
        module_states_[0].initialized = true;
        remaining_ms_ = static_cast<uint32_t>(60 + 30 * module_count_) * 1000u;
        last_tick_ = lv_tick_get();
        last_shown_seconds_ = (remaining_ms_ + 999) / 1000;
        feedback_remaining_ms_ = 0;
        screen_ = Screen::Playing;
        setup_module();
    }

    Module current_module() const
    {
        return modules_[module_index_];
    }

    void activate_module(int index)
    {
        // Each run has at most one of each puzzle type. Their puzzle fields
        // stay intact; save the shared input controls before changing modules.
        auto &previous = module_states_[module_index_];
        previous.selection = selection_;
        previous.input_count = input_count_;
        previous.digit_cursor = digit_cursor_;
        for (int i = 0; i < 3; ++i) {
            previous.directions[i] = direction_input_[i];
            previous.digits[i] = digit_input_[i];
        }
        for (int i = 0; i < 4; ++i) previous.glyph_selected[i] = glyph_selected_[i];

        module_index_ = index;
        module_picker_open_ = false;
        feedback_remaining_ms_ = 0;
        auto &next = module_states_[index];
        if (!next.initialized) {
            next.initialized = true;
            setup_module();
            return;
        }
        selection_ = next.selection;
        input_count_ = next.input_count;
        digit_cursor_ = next.digit_cursor;
        for (int i = 0; i < 3; ++i) {
            direction_input_[i] = next.directions[i];
            digit_input_[i] = next.digits[i];
        }
        for (int i = 0; i < 4; ++i) glyph_selected_[i] = next.glyph_selected[i];
        render_playing();
    }

    void setup_module()
    {
        selection_ = 0;
        input_count_ = 0;
        digit_cursor_ = 0;
        for (int &digit : digit_input_) digit = 0;
        for (bool &selected : glyph_selected_) selected = false;

        switch (current_module()) {
            case Module::PatchCables: setup_cables(); break;
            case Module::GlyphStack: setup_glyphs(); break;
            case Module::FrequencyDial: setup_dial(); break;
            case Module::ArrowRelay: setup_relay(); break;
            case Module::WordLock: setup_word_lock(); break;
            case Module::SimonHonks: setup_simon(); break;
        }
        render_playing();
    }

    void setup_cables()
    {
        for (int &colour : cable_colours_) colour = random_index(4);

        cable_answer_ = -1;
        if ((unit_code_ % 2) == 0) {
            for (int i = 0; i < 4; ++i) {
                if (cable_colours_[i] == 0) {
                    cable_answer_ = i;
                    break;
                }
            }
        }
        if (cable_answer_ < 0) {
            for (int i = 3; i >= 0; --i) {
                if (cable_colours_[i] == 1) {
                    cable_answer_ = i;
                    break;
                }
            }
        }
        if (cable_answer_ < 0) {
            int gold_count = 0;
            int gold_index = -1;
            for (int i = 0; i < 4; ++i) {
                if (cable_colours_[i] == 3) {
                    gold_count++;
                    gold_index = i;
                }
            }
            if (gold_count == 1) cable_answer_ = gold_index;
        }
        if (cable_answer_ < 0) cable_answer_ = 3;
    }

    void setup_glyphs()
    {
        glyph_column_ = random_index(4);
        int pool[] = { 0, 1, 2, 3, 4, 5 };
        for (int i = 5; i > 0; --i) {
            const int j = random_index(i + 1);
            const int swap = pool[i];
            pool[i] = pool[j];
            pool[j] = swap;
        }
        for (int i = 0; i < 4; ++i) glyph_visible_[i] = pool[i];

        for (int i = 0; i < 4; ++i) glyph_answer_[i] = glyph_visible_[i];
        for (int i = 0; i < 3; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                if (glyph_answer_[j] < glyph_answer_[i]) {
                    const int swap = glyph_answer_[i];
                    glyph_answer_[i] = glyph_answer_[j];
                    glyph_answer_[j] = swap;
                }
            }
        }
    }

    void setup_dial()
    {
        dial_colour_ = random_index(4);
        selection_ = 0;
        const int digit = unit_code_ % 10;
        const int bucket = digit <= 1 ? 0 : digit <= 4 ? 1 : digit <= 7 ? 2 : 3;
        dial_answer_ = kDialAnswers[dial_colour_][bucket];
    }

    void setup_relay()
    {
        for (int &direction : relay_source_) direction = random_index(4);
        const int tens = unit_code_ / 10;
        for (int i = 0; i < 3; ++i) {
            if (tens <= 2) relay_answer_[i] = (relay_source_[i] + 1) % 4;
            else if (tens <= 4) relay_answer_[i] = (relay_source_[i] + 2) % 4;
            else if (tens <= 6) relay_answer_[i] = (relay_source_[i] + 3) % 4;
            else relay_answer_[i] = relay_source_[2 - i];
        }
    }

    void setup_word_lock()
    {
        word_index_ = random_index(8);
        const int parity = unit_code_ % 2;
        const int answer = kLockAnswers[word_index_][parity];
        digit_answer_[0] = answer / 100;
        digit_answer_[1] = (answer / 10) % 10;
        digit_answer_[2] = answer % 10;
    }

    void setup_simon()
    {
        for (int &colour : simon_sequence_) colour = random_index(4);
        const int digit = unit_code_ % 10;
        const int offset = digit <= 2 ? 0 : digit <= 5 ? 1 : digit <= 7 ? 2 : 3;
        for (int i = 0; i < 3; ++i) simon_answer_[i] = (simon_sequence_[i] + offset) % 4;
        simon_started_at_ = lv_tick_get();
        simon_phase_ = -1;
    }

    void render_playing()
    {
        reset_root();

        timer_label_ = make_label("", &lv_font_montserrat_20, kGold);
        lv_obj_set_pos(timer_label_, 8, 4);
        update_timer();

        lv_obj_t *unit = make_label("", &lv_font_montserrat_14,
                                    BadgeTheme::ACCENT_DETAIL);
        lv_label_set_text_fmt(unit, "UNIT %02d", unit_code_);
        lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 8);

        lv_obj_t *progress = make_label("", &lv_font_montserrat_14,
                                        BadgeTheme::TEXT_SOFT);
        lv_label_set_text_fmt(progress, "%d/%d", solved_count_, module_count_);
        lv_obj_align(progress, LV_ALIGN_TOP_RIGHT, -9, 8);

        lv_obj_t *rule = make_box(8, 31, 304, 2, BadgeTheme::ACCENT);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_radius(rule, 0, 0);

        if (module_picker_open_) {
            render_module_picker();
            return;
        }

        lv_obj_t *title = make_label("", &lv_font_montserrat_18);
        lv_label_set_text_fmt(title, "MODULE %d", module_index_ + 1);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 39);

        switch (current_module()) {
            case Module::PatchCables:   render_cables(); break;
            case Module::GlyphStack:    render_glyphs(); break;
            case Module::FrequencyDial: render_dial(); break;
            case Module::ArrowRelay:    render_relay(); break;
            case Module::WordLock:      render_word_lock(); break;
            case Module::SimonHonks:    render_simon(); break;
        }

        feedback_label_ = make_label("", &lv_font_montserrat_14,
                                     BadgeTheme::TEXT_MUTED);
        lv_obj_set_width(feedback_label_, 310);
        lv_obj_set_style_text_align(feedback_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(feedback_label_, LV_ALIGN_BOTTOM_MID, 0, -23);
        update_feedback();

        lv_obj_t *module_hint = make_label("START: modules",
                                           &lv_font_montserrat_12,
                                           BadgeTheme::TEXT_DIM);
        lv_obj_align(module_hint, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    void render_module_picker()
    {
        lv_obj_t *title = make_label("MODULES", &lv_font_montserrat_18);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 39);
        for (int i = 0; i < module_count_; ++i) {
            const bool solved = module_states_[i].solved;
            const bool focused = i == module_choice_;
            lv_obj_t *box = make_box(20 + (i % 3) * 96, 76 + (i / 3) * 66,
                                     88, 54, focused ? BadgeTheme::TRACK : BadgeTheme::PANEL);
            lv_obj_set_style_border_width(box, focused ? 3 : 1, 0);
            lv_obj_set_style_border_color(box,
                lv_color_hex(focused ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);
            lv_obj_t *number = lv_label_create(box);
            lv_label_set_text_fmt(number, "%d", i + 1);
            lv_obj_set_style_text_font(number, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(number, lv_color_hex(solved ? kGreen : BadgeTheme::TEXT), 0);
            lv_obj_align(number, LV_ALIGN_TOP_MID, 0, 3);
            lv_obj_t *status = lv_label_create(box);
            lv_label_set_text(status, solved ? "SOLVED" : "OPEN");
            lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(status,
                lv_color_hex(solved ? kGreen : BadgeTheme::TEXT_MUTED), 0);
            lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -4);
        }
    }

    void handle_module_picker(Button button)
    {
        if (button == Button::B || button == Button::Start) {
            module_picker_open_ = false;
            render_playing();
            return;
        }
        if (button == Button::A) {
            if (!module_states_[module_choice_].solved) activate_module(module_choice_);
            return;
        }
        const int old = module_choice_;
        switch (button) {
            case Button::Left:  if (module_choice_ > 0) --module_choice_; break;
            case Button::Right: if (module_choice_ + 1 < module_count_) ++module_choice_; break;
            case Button::Up:    if (module_choice_ >= 3) module_choice_ -= 3; break;
            case Button::Down:  if (module_choice_ + 3 < module_count_) module_choice_ += 3; break;
            default: return;
        }
        if (module_choice_ != old) render_playing();
    }

    void render_cables()
    {
        lv_obj_t *prompt = make_label("Describe all four cables. Cut exactly one.",
                                      &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 66);
        for (int i = 0; i < 4; ++i) {
            lv_obj_t *box = make_box(14 + i * 76, 91, 64, 79);
            lv_obj_set_style_border_width(box, i == selection_ ? 3 : 1, 0);
            lv_obj_set_style_border_color(box,
                lv_color_hex(i == selection_ ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);

            lv_obj_t *number = lv_label_create(box);
            lv_label_set_text_fmt(number, "%d", i + 1);
            lv_obj_set_style_text_font(number, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(number, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_align(number, LV_ALIGN_TOP_MID, 0, 5);

            lv_obj_t *cable = lv_obj_create(box);
            lv_obj_set_size(cable, 42, 10);
            lv_obj_set_style_radius(cable, 5, 0);
            lv_obj_set_style_border_width(cable, 0, 0);
            lv_obj_set_style_bg_opa(cable, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(cable, lv_color_hex(kColours[cable_colours_[i]]), 0);
            lv_obj_align(cable, LV_ALIGN_CENTER, 0, 2);

            lv_obj_t *name = lv_label_create(box);
            lv_label_set_text(name, kColourNames[cable_colours_[i]]);
            lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(name, lv_color_hex(kColours[cable_colours_[i]]), 0);
            lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -5);
        }
    }

    void render_glyphs()
    {
        lv_obj_t *prompt = make_label("Experts know the press order.",
                                      &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 67);
        for (int i = 0; i < 4; ++i) {
            const bool focused = i == selection_;
            lv_obj_t *box = make_box(18 + i * 75, 96, 59, 62,
                                     glyph_selected_[i] ? BadgeTheme::TRACK : BadgeTheme::PANEL);
            lv_obj_set_style_border_width(box, focused ? 3 : 1, 0);
            lv_obj_set_style_border_color(box,
                lv_color_hex(focused ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);
            lv_obj_t *glyph = lv_label_create(box);
            lv_label_set_text(glyph, kGlyphColumns[glyph_column_][glyph_visible_[i]]);
            lv_obj_set_style_text_font(glyph, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(glyph,
                lv_color_hex(glyph_selected_[i] ? BadgeTheme::TEXT_DIM : BadgeTheme::TEXT), 0);
            lv_obj_center(glyph);
        }
        lv_obj_t *progress = make_label("", &lv_font_montserrat_14, kGold);
        lv_label_set_text_fmt(progress, "%d of 4 entered", input_count_);
        lv_obj_align(progress, LV_ALIGN_TOP_MID, 0, 168);
    }

    void render_dial()
    {
        lv_obj_t *prompt = make_label("Signal colour", &lv_font_montserrat_14,
                                      BadgeTheme::TEXT_SOFT);
        lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 67);
        lv_obj_t *colour = make_label(kColourNames[dial_colour_], &lv_font_montserrat_20,
                                      kColours[dial_colour_]);
        lv_obj_align(colour, LV_ALIGN_TOP_MID, 0, 87);

        lv_obj_t *dial = make_box(113, 120, 94, 59, BadgeTheme::SURFACE);
        lv_obj_set_style_border_width(dial, 3, 0);
        lv_obj_set_style_border_color(dial, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_t *value = lv_label_create(dial);
        lv_label_set_text_fmt(value, "< %d >", selection_);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(value, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_center(value);
    }

    void render_relay()
    {
        lv_obj_t *prompt = make_label("Transform this transmission:",
                                      &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 67);
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *source = make_box(71 + i * 63, 92, 52, 49, BadgeTheme::SURFACE);
            lv_obj_t *letter = lv_label_create(source);
            lv_label_set_text(letter, kDirections[relay_source_[i]]);
            lv_obj_set_style_text_font(letter, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(letter, lv_color_hex(kBlue), 0);
            lv_obj_center(letter);

            lv_obj_t *entry = make_box(71 + i * 63, 151, 52, 38,
                                       i < input_count_ ? BadgeTheme::TRACK : BadgeTheme::PANEL);
            lv_obj_t *answer = lv_label_create(entry);
            lv_label_set_text(answer, i < input_count_ ? kDirections[direction_input_[i]] : "-");
            lv_obj_set_style_text_font(answer, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(answer, lv_color_hex(kGold), 0);
            lv_obj_center(answer);
        }
    }

    void render_word_lock()
    {
        lv_obj_t *word = make_label(kLockWords[word_index_], &lv_font_montserrat_24, kGreen);
        lv_obj_align(word, LV_ALIGN_TOP_MID, 0, 69);
        for (int i = 0; i < 3; ++i) {
            lv_obj_t *box = make_box(75 + i * 61, 112, 49, 63, BadgeTheme::SURFACE);
            lv_obj_set_style_border_width(box, i == digit_cursor_ ? 3 : 1, 0);
            lv_obj_set_style_border_color(box,
                lv_color_hex(i == digit_cursor_ ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::BORDER), 0);
            lv_obj_t *digit = lv_label_create(box);
            lv_label_set_text_fmt(digit, "%d", digit_input_[i]);
            lv_obj_set_style_text_font(digit, &lv_font_montserrat_24, 0);
            lv_obj_set_style_text_color(digit, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_center(digit);
        }
    }

    void render_simon()
    {
        lv_obj_t *prompt = make_label("Watch the three-colour loop.",
                                      &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        lv_obj_align(prompt, LV_ALIGN_TOP_MID, 0, 67);

        simon_light_ = make_box(97, 91, 126, 48, BadgeTheme::TRACK);
        lv_obj_set_style_border_width(simon_light_, 2, 0);
        simon_colour_label_ = lv_label_create(simon_light_);
        lv_label_set_text(simon_colour_label_, "...");
        lv_obj_set_style_text_font(simon_colour_label_, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(simon_colour_label_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_center(simon_colour_label_);

        for (int i = 0; i < 3; ++i) {
            lv_obj_t *entry = make_box(85 + i * 51, 151, 42, 38,
                                       i < input_count_ ? BadgeTheme::TRACK : BadgeTheme::PANEL);
            lv_obj_t *answer = lv_label_create(entry);
            lv_label_set_text(answer, i < input_count_ ? kDirections[direction_input_[i]] : "-");
            lv_obj_set_style_text_font(answer, &lv_font_montserrat_18, 0);
            lv_obj_set_style_text_color(answer, lv_color_hex(kGold), 0);
            lv_obj_center(answer);
        }
        simon_phase_ = -1;
        update_simon(lv_tick_get());
    }

    void update_timer()
    {
        if (!timer_label_) return;
        const uint32_t seconds = (remaining_ms_ + 999) / 1000;
        lv_label_set_text_fmt(timer_label_, "%u:%02u",
                              static_cast<unsigned>(seconds / 60),
                              static_cast<unsigned>(seconds % 60));
        lv_obj_set_style_text_color(timer_label_,
            lv_color_hex(seconds <= 20 ? kRed : kGold), 0);
    }

    const char *default_hint() const
    {
        switch (current_module()) {
            case Module::PatchCables:   return "L/R: select   A: cut";
            case Module::GlyphStack:    return "L/R: select   A: press   B: clear";
            case Module::FrequencyDial: return "L/R: dial   A: submit";
            case Module::ArrowRelay:    return "D-pad: enter   B: clear";
            case Module::WordLock:      return "L/R: digit   U/D: value   A: submit";
            case Module::SimonHonks:    return "D-pad: enter   A: replay   B: clear";
        }
        return "";
    }

    void update_feedback()
    {
        if (!feedback_label_) return;
        if (feedback_remaining_ms_ > 0) {
            lv_label_set_text(feedback_label_, "WRONG - 10 SECONDS LOST");
            lv_obj_set_style_text_color(feedback_label_, lv_color_hex(kRed), 0);
        } else {
            lv_label_set_text(feedback_label_, default_hint());
            lv_obj_set_style_text_color(feedback_label_,
                                        lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        }
    }

    void update_simon(uint32_t now)
    {
        if (!simon_light_ || !simon_colour_label_) return;
        const uint32_t elapsed = now - simon_started_at_;
        const int slot = static_cast<int>((elapsed / 600u) % 4u);
        const bool lit = slot < 3 && (elapsed % 600u) < 420u;
        const int phase = lit ? slot : -1;
        if (phase == simon_phase_) return;
        simon_phase_ = phase;

        if (phase < 0) {
            lv_obj_set_style_bg_color(simon_light_, lv_color_hex(BadgeTheme::TRACK), 0);
            lv_obj_set_style_border_color(simon_light_, lv_color_hex(BadgeTheme::BORDER), 0);
            lv_label_set_text(simon_colour_label_, "...");
            lv_obj_set_style_text_color(simon_colour_label_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        } else {
            const int colour = simon_sequence_[phase];
            lv_obj_set_style_bg_color(simon_light_, lv_color_hex(kColours[colour]), 0);
            lv_obj_set_style_border_color(simon_light_, lv_color_hex(kColours[colour]), 0);
            lv_label_set_text(simon_colour_label_, kColourNames[colour]);
            lv_obj_set_style_text_color(simon_colour_label_, lv_color_hex(0x050505), 0);
        }
    }

    void handle_module_button(Button button)
    {
        if (module_picker_open_) {
            handle_module_picker(button);
            return;
        }
        if (button == Button::Start) {
            module_choice_ = module_index_;
            module_picker_open_ = true;
            render_playing();
            return;
        }
        if (button == Button::B) {
            clear_current_input();
            render_playing();
            return;
        }

        switch (current_module()) {
            case Module::PatchCables:   handle_cables(button); break;
            case Module::GlyphStack:    handle_glyphs(button); break;
            case Module::FrequencyDial: handle_dial(button); break;
            case Module::ArrowRelay:    handle_relay(button); break;
            case Module::WordLock:      handle_word_lock(button); break;
            case Module::SimonHonks:    handle_simon(button); break;
        }
    }

    void handle_cables(Button button)
    {
        if (button == Button::Left) selection_ = (selection_ + 3) % 4;
        else if (button == Button::Right) selection_ = (selection_ + 1) % 4;
        else if (button == Button::A) {
            submit(selection_ == cable_answer_);
            return;
        } else return;
        render_playing();
    }

    void handle_glyphs(Button button)
    {
        if (button == Button::Left) selection_ = (selection_ + 3) % 4;
        else if (button == Button::Right) selection_ = (selection_ + 1) % 4;
        else if (button == Button::A) {
            if (glyph_selected_[selection_]) return;
            if (glyph_visible_[selection_] != glyph_answer_[input_count_]) {
                submit(false);
                return;
            }
            glyph_selected_[selection_] = true;
            input_count_++;
            if (input_count_ == 4) {
                submit(true);
                return;
            }
        } else return;
        render_playing();
    }

    void handle_dial(Button button)
    {
        if (button == Button::Left) selection_ = (selection_ + 9) % 10;
        else if (button == Button::Right) selection_ = (selection_ + 1) % 10;
        else if (button == Button::A) {
            submit(selection_ == dial_answer_);
            return;
        } else return;
        render_playing();
    }

    int direction_from_button(Button button) const
    {
        if (button == Button::Up) return 0;
        if (button == Button::Right) return 1;
        if (button == Button::Down) return 2;
        if (button == Button::Left) return 3;
        return -1;
    }

    void handle_relay(Button button)
    {
        const int direction = direction_from_button(button);
        if (direction < 0) return;
        direction_input_[input_count_++] = direction;
        if (input_count_ == 3) {
            bool correct = true;
            for (int i = 0; i < 3; ++i) correct &= direction_input_[i] == relay_answer_[i];
            submit(correct);
            return;
        }
        render_playing();
    }

    void handle_word_lock(Button button)
    {
        if (button == Button::Left) digit_cursor_ = (digit_cursor_ + 2) % 3;
        else if (button == Button::Right) digit_cursor_ = (digit_cursor_ + 1) % 3;
        else if (button == Button::Up) digit_input_[digit_cursor_] = (digit_input_[digit_cursor_] + 1) % 10;
        else if (button == Button::Down) digit_input_[digit_cursor_] = (digit_input_[digit_cursor_] + 9) % 10;
        else if (button == Button::A) {
            bool correct = true;
            for (int i = 0; i < 3; ++i) correct &= digit_input_[i] == digit_answer_[i];
            submit(correct);
            return;
        } else return;
        render_playing();
    }

    void handle_simon(Button button)
    {
        if (button == Button::A) {
            simon_started_at_ = lv_tick_get();
            simon_phase_ = -1;
            update_simon(simon_started_at_);
            return;
        }
        const int direction = direction_from_button(button);
        if (direction < 0) return;
        direction_input_[input_count_++] = direction;
        if (input_count_ == 3) {
            bool correct = true;
            for (int i = 0; i < 3; ++i) correct &= direction_input_[i] == simon_answer_[i];
            submit(correct);
            return;
        }
        render_playing();
    }

    void clear_current_input()
    {
        input_count_ = 0;
        for (bool &selected : glyph_selected_) selected = false;
        for (int &digit : digit_input_) digit = 0;
        digit_cursor_ = 0;
    }

    void submit(bool correct)
    {
        if (!correct) {
            if (remaining_ms_ <= kWrongPenaltyMs) {
                remaining_ms_ = 0;
                screen_ = Screen::Lost;
                render();
                return;
            }
            remaining_ms_ -= kWrongPenaltyMs;
            last_shown_seconds_ = (remaining_ms_ + 999) / 1000;
            feedback_remaining_ms_ = kFeedbackMs;
            clear_current_input();
            render_playing();
            return;
        }

        module_states_[module_index_].solved = true;
        solved_count_++;
        feedback_remaining_ms_ = 0;
        if (solved_count_ == module_count_) {
            screen_ = Screen::Won;
            render();
        } else {
            for (int step = 1; step < module_count_; ++step) {
                const int next = (module_index_ + step) % module_count_;
                if (!module_states_[next].solved) {
                    activate_module(next);
                    break;
                }
            }
        }
    }

    void render_result()
    {
        reset_root();
        const bool won = screen_ == Screen::Won;
        const uint32_t colour = won ? kGreen : kRed;

        lv_obj_t *mark = make_box(119, 26, 82, 82, won ? 0x123c2b : 0x3d1720);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(mark, 3, 0);
        lv_obj_set_style_border_color(mark, lv_color_hex(colour), 0);
        lv_obj_t *symbol = lv_label_create(mark);
        lv_label_set_text(symbol, won ? "OK" : "HNK");
        lv_obj_set_style_text_font(symbol, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(symbol, lv_color_hex(colour), 0);
        lv_obj_center(symbol);

        lv_obj_t *title = make_label(won ? "DEFUSED!" : "TOO MANY HONKS",
                                     &lv_font_montserrat_24, colour);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 120);

        lv_obj_t *detail = make_label("", &lv_font_montserrat_14,
                                      BadgeTheme::TEXT_SOFT);
        if (won) {
            const uint32_t seconds = (remaining_ms_ + 999) / 1000;
            lv_label_set_text_fmt(detail, "%d modules with %u:%02u remaining",
                                  module_count_, static_cast<unsigned>(seconds / 60),
                                  static_cast<unsigned>(seconds % 60));
        } else {
            lv_label_set_text_fmt(detail, "%d of %d modules solved",
                                  solved_count_, module_count_);
        }
        lv_obj_align(detail, LV_ALIGN_TOP_MID, 0, 157);

        lv_obj_t *hint = make_label("START: REMATCH   B: READY ROOM",
                                    &lv_font_montserrat_14, BadgeTheme::ACCENT_DETAIL);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -13);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *timer_label_ = nullptr;
    lv_obj_t *feedback_label_ = nullptr;
    lv_obj_t *simon_light_ = nullptr;
    lv_obj_t *simon_colour_label_ = nullptr;

    Screen screen_ = Screen::Briefing;
    Module modules_[6] = {};
    struct ModuleState {
        bool initialized = false;
        bool solved = false;
        int selection = 0;
        int input_count = 0;
        int digit_cursor = 0;
        int directions[3] = {};
        int digits[3] = {};
        bool glyph_selected[4] = {};
    };
    ModuleState module_states_[6] = {};
    int module_count_ = 3;
    int module_index_ = 0;
    int solved_count_ = 0;
    int module_choice_ = 0;
    bool module_picker_open_ = false;
    int unit_code_ = 10;

    uint32_t rng_state_ = 0;
    uint32_t remaining_ms_ = 0;
    uint32_t last_tick_ = 0;
    uint32_t last_shown_seconds_ = 0;
    uint32_t feedback_remaining_ms_ = 0;

    int selection_ = 0;
    int input_count_ = 0;
    int direction_input_[3] = {};

    int cable_colours_[4] = {};
    int cable_answer_ = 0;

    int glyph_column_ = 0;
    int glyph_visible_[4] = {};
    int glyph_answer_[4] = {};
    bool glyph_selected_[4] = {};

    int dial_colour_ = 0;
    int dial_answer_ = 0;

    int relay_source_[3] = {};
    int relay_answer_[3] = {};

    int word_index_ = 0;
    int digit_cursor_ = 0;
    int digit_input_[3] = {};
    int digit_answer_[3] = {};

    int simon_sequence_[3] = {};
    int simon_answer_[3] = {};
    uint32_t simon_started_at_ = 0;
    int simon_phase_ = -1;
};

KtnhApp s_instance;

} // namespace

App *app_ktnh_instance() { return &s_instance; }
