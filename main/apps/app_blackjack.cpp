#include "app.h"
#include "blackjack_rules.h"
#include "hal_config.h"
#include "hal_lights.h"

#include "esp_random.h"
#include <algorithm>
#include <climits>
#include <cstdio>

class BlackjackApp : public GameApp {
public:
    const char *name() const override { return "Blackjack"; }
    const char *icon() const override { return "21"; }
    const char *slug() const override { return "blackjack"; }
    AppAccess access() const override { return AppAccess::Public; }

    void on_enter(lv_obj_t *root) override
    {
        wins_ = std::max(0, hal_config_get_int(slug(), "wins", 0));
        losses_ = std::max(0, hal_config_get_int(slug(), "losses", 0));
        tie_wins_ = std::max(0, hal_config_get_int(slug(), "tie_wins", 0));
        label(root, "Blackjack", 5, &lv_font_montserrat_20, BadgeTheme::TEXT);
        stats_ = label(root, "", 30, &lv_font_montserrat_12, BadgeTheme::TEXT_MUTED);
        dealer_ = label(root, "", 53, &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        dealer_cards_ = label(root, "", 74, &lv_font_montserrat_20, BadgeTheme::TEXT);
        player_ = label(root, "", 110, &lv_font_montserrat_14, BadgeTheme::TEXT_SOFT);
        player_cards_ = label(root, "", 131, &lv_font_montserrat_20, BadgeTheme::TEXT);
        status_ = label(root, "", 165, &lv_font_montserrat_16, BadgeTheme::ACCENT_DETAIL);
        label(root, "Ties win. Dealer stands on 17.", 191,
              &lv_font_montserrat_12, BadgeTheme::TEXT_MUTED);
        hint_ = label(root, "", 215, &lv_font_montserrat_14, BadgeTheme::TEXT);
        deal();
    }

    void on_exit() override
    {
        stats_ = dealer_ = dealer_cards_ = player_ = player_cards_ = status_ = hint_ = nullptr;
        hal_lights_clear();
        hal_lights_show();
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (round_.result() != Blackjack::Result::Playing) {
            if (ev.button == Button::A || ev.button == Button::Start) deal();
            return;
        }
        if (ev.button == Button::A) round_.hit();
        else if (ev.button == Button::B) round_.stand();
        else return;
        record_result();
        render();
    }

private:
    static lv_obj_t *label(lv_obj_t *root, const char *text, int y,
                           const lv_font_t *font, uint32_t color)
    {
        auto *obj = lv_label_create(root);
        lv_label_set_text(obj, text);
        lv_obj_set_width(obj, 304);
        lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(obj, font, 0);
        lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
        lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
        return obj;
    }

    void deal()
    {
        hal_lights_clear();
        hal_lights_show();
        Blackjack::Deck deck;
        for (unsigned i = 0; i < deck.size(); ++i) deck[i] = i;
        for (unsigned i = deck.size() - 1; i > 0; --i)
            std::swap(deck[i], deck[esp_random() % (i + 1)]);
        recorded_ = false;
        round_.deal(deck);
        record_result();
        render();
    }

    void increment(const char *key, int &value)
    {
        if (value < INT_MAX) hal_config_set_int(slug(), key, ++value);
    }

    void record_result()
    {
        const auto result = round_.result();
        if (recorded_ || result == Blackjack::Result::Playing) return;
        recorded_ = true;
        if (result == Blackjack::Result::Loss) {
            increment("losses", losses_);
            for (size_t i = 0; i < LED_COUNT; ++i) hal_lights_set(i, 255, 0, 0);
            hal_lights_show();
        } else {
            increment("wins", wins_);
            if (result == Blackjack::Result::TieWin) increment("tie_wins", tie_wins_);
            hal_lights_flash_success();
        }
    }

    static void show_cards(lv_obj_t *label, const Blackjack::Hand &hand, bool hidden)
    {
        static const char *ranks[] = {"A", "2", "3", "4", "5", "6", "7",
                                      "8", "9", "10", "J", "Q", "K"};
        char text[48] = {};
        size_t used = 0;
        for (unsigned i = 0; i < hand.count; ++i) {
            const char *rank = hidden && i > 0 ? "?" : ranks[hand.cards[i] % 13];
            used += snprintf(text + used, sizeof(text) - used, "%s%s", i ? " " : "", rank);
        }
        // Eleven-card hands fit on one line without covering the result.
        lv_obj_set_style_text_font(label, hand.count > 7 ? &lv_font_montserrat_14
                                                       : &lv_font_montserrat_20, 0);
        lv_label_set_text(label, text);
    }

    void render()
    {
        const auto result = round_.result();
        const bool playing = result == Blackjack::Result::Playing;
        lv_label_set_text_fmt(stats_, "Wins %d  Losses %d  Ties %d", wins_, losses_, tie_wins_);
        if (playing) lv_label_set_text(dealer_, "Dealer");
        else lv_label_set_text_fmt(dealer_, "Dealer: %d", round_.dealer().total());
        lv_label_set_text_fmt(player_, "You: %d", round_.player().total());
        show_cards(dealer_cards_, round_.dealer(), playing);
        show_cards(player_cards_, round_.player(), false);
        const char *message = "Hit or stand?";
        if (result == Blackjack::Result::TieWin) message = "Tie - you win!";
        else if (result == Blackjack::Result::Loss)
            message = round_.player().total() > 21 ? "Bust - dealer wins" : "Dealer wins";
        else if (result == Blackjack::Result::Win)
            message = round_.player().count == 2 && round_.player().total() == 21
                ? "Blackjack - you win!"
                : round_.dealer().total() > 21 ? "Dealer busts - you win!" : "You win!";
        lv_label_set_text(status_, message);
        lv_label_set_text(hint_, playing ? "A: Hit     B: Stand" : "A / Start: Deal again");
    }

    Blackjack::Round round_;
    bool recorded_ = false;
    int wins_ = 0, losses_ = 0, tie_wins_ = 0;
    lv_obj_t *stats_ = nullptr, *dealer_ = nullptr, *dealer_cards_ = nullptr;
    lv_obj_t *player_ = nullptr, *player_cards_ = nullptr, *status_ = nullptr, *hint_ = nullptr;
};

static BlackjackApp s_instance;
App *app_blackjack_instance() { return &s_instance; }
