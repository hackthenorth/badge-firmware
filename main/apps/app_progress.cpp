#include "app.h"
#include "app_registry.h"

extern "C" const lv_image_dsc_t icon_progress;

#include "launcher_slugs.h"
#include "pal_picker.h"
#include "sponsor_layout.h"
#include "sponsor_store.h"

#include <cstdio>


// "How much of the event have I done?"
//
// Numerators are derived locally and denominators come from firmware defaults
// or local event-day config, without waiting for an upload-station sync.
// This app has no server dependency at all.

static constexpr int kScavengerHuntTotal = 9;

// Only attendee-facing public apps count toward event progress. Internal tools
// are deliberately excluded even when an engineer has revealed them.
static int apps_total()
{
    int n = 0;
    for (size_t i = 0; i < app_registry_count(); i++) {
        App *a = app_registry_get(i);
        if (a && a->access() == AppAccess::Public) n++;
    }
    return n;
}

static int apps_found()
{
    int n = 0;
    for (size_t i = 0; i < launcher_slugs_count(); i++) {
        App *a = app_registry_find_by_slug(launcher_slugs_at(i));
        if (a && a->access() == AppAccess::Public) n++;
    }
    return n;
}

class ProgressApp : public App {
public:
    const char *name() const override { return "Progress"; }
    const char *icon() const override { return "%"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_progress; }
    const char *slug() const override { return "progress"; }
    AppAccess access() const override { return AppAccess::Default; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        showing_clues_ = false;
        render();
    }

    void on_exit() override { root_ = arc_ = nullptr; }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (ev.button == Button::B) {
            if (!showing_clues_) return;
            showing_clues_ = false;
        } else if (ev.button == Button::A || ev.button == Button::Start
                   || ev.button == Button::Left || ev.button == Button::Right) {
            showing_clues_ = !showing_clues_;
        } else {
            return;
        }
        render();
    }

private:
    void render()
    {
        lv_obj_clean(root_);
        arc_ = nullptr;
        if (showing_clues_) render_clues();
        else render_overview();

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, showing_clues_
            ? "B  Progress     Home  Apps"
            : "A  Clues     Home  Apps");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    void render_clues()
    {
        lv_obj_t *title = lv_label_create(root_);
        lv_label_set_text(title, "Clues");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        char app_count[24];
        char pal_count[24];
        snprintf(app_count, sizeof(app_count), "%d/%d", apps_found(), apps_total());
        snprintf(pal_count, sizeof(pal_count), "%d/%d",
                 pal_unlocked_count(), kScavengerHuntTotal);
        struct Clue { const char *action; const char *status; bool earned; };
        const bool conversation = launcher_slugs_contains("conversation");
        const bool bubble_wrap = launcher_slugs_contains("bubblewrap");
        const Clue clues[] = {
            { "Connect + first bump",
              conversation ? "Unlocked" : "Locked", conversation },
            { "QR + Konami",
              bubble_wrap ? "Unlocked" : "Locked", bubble_wrap },
            { "Scanner + app sticker", app_count,
              apps_total() > 0 && apps_found() >= apps_total() },
            { "Find Solana boxes", pal_count,
              pal_unlocked_count() >= kScavengerHuntTotal },
        };
        for (size_t i = 0; i < sizeof(clues) / sizeof(clues[0]); ++i) {
            const Clue &clue = clues[i];
            lv_obj_t *row = lv_obj_create(root_);
            lv_obj_set_size(row, 300, 39);
            lv_obj_set_pos(row, 10, 39 + static_cast<int>(i) * 44);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_set_style_radius(row, 7, 0);
            lv_obj_set_style_border_width(row, 1, 0);
            lv_obj_set_style_border_color(row, lv_color_hex(BadgeTheme::BORDER), 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(BadgeTheme::SURFACE), 0);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *action = lv_label_create(row);
            lv_label_set_text(action, clue.action);
            lv_obj_set_style_text_font(action, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(action, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_align(action, LV_ALIGN_LEFT_MID, 8, 0);

            lv_obj_t *status = lv_label_create(row);
            lv_label_set_text(status, clue.status);
            lv_obj_set_style_text_font(status, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(status, lv_color_hex(clue.earned
                ? 0x36e07a : BadgeTheme::TEXT_DIM), 0);
            lv_obj_align(status, LV_ALIGN_RIGHT_MID, -8, 0);
        }
    }

    void render_overview()
    {
        struct Row { const char *label; int have; int total; };
        const Row rows[] = {
            { "Apps",        apps_found(),      apps_total()    },
            { "Sponsors",    static_cast<int>(sponsor_store_count()),
                              static_cast<int>(sponsor_layout_table_count()) },
            { "Solana\nScavenger Hunt", pal_unlocked_count(), kScavengerHuntTotal },
        };
        constexpr int kRowCount = (int)(sizeof(rows) / sizeof(rows[0]));

        int done = 0, total = 0;
        for (const auto &r : rows) {
            done  += (r.have > r.total) ? r.total : r.have;
            total += r.total;
        }
        int pct = total > 0 ? (done * 100) / total : 0;

        // Center the progress summary above the footer.
        lv_obj_t *content = lv_obj_create(root_);
        lv_obj_remove_style_all(content);
        lv_obj_set_size(content, 292, 170);
        lv_obj_align(content, LV_ALIGN_CENTER, 0, -12);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(content);
        lv_label_set_text(title, "Progress");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

        arc_ = lv_arc_create(content);
        lv_obj_set_size(arc_, 92, 92);
        lv_arc_set_rotation(arc_, 270);
        lv_arc_set_bg_angles(arc_, 0, 360);
        lv_arc_set_range(arc_, 0, 100);
        lv_arc_set_value(arc_, pct);
        lv_obj_remove_style(arc_, nullptr, LV_PART_KNOB);
        lv_obj_clear_flag(arc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(arc_, lv_color_hex(BadgeTheme::TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), LV_PART_INDICATOR);
        lv_obj_align(arc_, LV_ALIGN_TOP_LEFT, 0, 34);

        lv_obj_t *big = lv_label_create(content);
        lv_label_set_text_fmt(big, "%d/%d", done, total);
        lv_obj_set_style_text_font(big, &lv_font_montserrat_22, 0);
        lv_obj_set_style_text_color(big, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align_to(big, arc_, LV_ALIGN_CENTER, 0, 0);

        int y = 34;
        for (int i = 0; i < kRowCount; i++) {
            const Row &r = rows[i];
            lv_obj_t *lb = lv_label_create(content);
            lv_obj_set_width(lb, 182);
            lv_label_set_long_mode(lb, LV_LABEL_LONG_WRAP);
            lv_label_set_text_fmt(lb, "%s  %d/%d", r.label, r.have, r.total);
            lv_obj_set_style_text_font(lb, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(lb, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
            lv_obj_align(lb, LV_ALIGN_TOP_LEFT, 110, y);
            lv_obj_update_layout(lb);
            const int label_height = lv_obj_get_height(lb);

            lv_obj_t *bar = lv_bar_create(content);
            lv_obj_set_size(bar, 182, 6);
            lv_bar_set_range(bar, 0, r.total > 0 ? r.total : 1);
            lv_bar_set_value(bar, r.have, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(BadgeTheme::TRACK), LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar, lv_color_hex(BadgeTheme::ACCENT_DETAIL), LV_PART_INDICATOR);
            lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 110, y + label_height + 4);
            y += label_height + 20;
        }

    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *arc_ = nullptr;
    bool showing_clues_ = false;
};

static ProgressApp s_instance;
App *app_progress_instance() { return &s_instance; }
