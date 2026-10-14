#include "app_workshops.h"

#include "app.h"
#include "hal_lights.h"
#include "hal_nfc.h"
#include "workshop_catalog.h"
#include "workshop_store.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" const lv_image_dsc_t icon_workshops;

namespace {

WorkshopVisitResult s_pending_visit;
bool s_has_pending_visit = false;

constexpr size_t kColumns = 5;
constexpr size_t kRows = 3;
constexpr size_t kPerPage = kColumns * kRows;
constexpr int kHexWidth = 58;
constexpr int kHexHeight = 40;
constexpr int kHexX = 8;
constexpr int kHexY = 43;
constexpr int kHexPitchX = 61;
constexpr int kHexPitchY = 49;
constexpr int kColumnStagger = 22;
constexpr int kNoticeTicks = 100;
constexpr int kLightTicks = 40;
constexpr size_t kCatalogCapacity = 48;

constexpr uint32_t kCollectedColors[] = {
    0x18cfa7,
    0x4d8df7,
    0x9b6cf4,
    0xee5d9f,
    0xf4a950,
};

class WorkshopsApp final : public App {
public:
    const char *name() const override { return "Workshop Quest"; }
    const char *icon() const override { return "HEX"; }
    const char *slug() const override { return "workshops"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_workshops; }
    AppAccess access() const override { return AppAccess::Default; }
    // Entry always reboots first (registry): the quest + NFC session wants a
    // fresh heap, and exit reboots back for the same reason.
    bool uses_nfc() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        selected_ = 0;
        page_ = 0;
        notice_[0] = '\0';
        notice_ticks_ = 0;
        light_ticks_ = 0;

        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        playfield_ = lv_obj_create(root_);
        lv_obj_remove_style_all(playfield_);
        lv_obj_set_size(playfield_, 320, 240);
        lv_obj_set_pos(playfield_, 0, 0);
        lv_obj_clear_flag(playfield_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(playfield_, draw_event, LV_EVENT_DRAW_MAIN, this);

        for (size_t slot = 0; slot < kPerPage; ++slot) {
            const int column = static_cast<int>(slot % kColumns);
            const int row = static_cast<int>(slot / kColumns);
            const int x = kHexX + column * kHexPitchX;
            const int y = kHexY + row * kHexPitchY
                + (column % 2) * kColumnStagger;

            cell_labels_[slot] = lv_label_create(playfield_);
            lv_obj_remove_style_all(cell_labels_[slot]);
            lv_obj_set_size(cell_labels_[slot], kHexWidth, 18);
            lv_obj_set_pos(cell_labels_[slot], x, y + 11);
            lv_obj_set_style_text_align(cell_labels_[slot], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(cell_labels_[slot], &lv_font_montserrat_12, 0);
        }

        title_label_ = lv_label_create(root_);
        lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(title_label_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(title_label_, LV_ALIGN_TOP_LEFT, 8, 4);

        progress_label_ = lv_label_create(root_);
        lv_obj_set_style_text_font(progress_label_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(progress_label_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(progress_label_, LV_ALIGN_TOP_RIGHT, -8, 4);

        name_label_ = lv_label_create(root_);
        lv_obj_set_width(name_label_, 304);
        lv_label_set_long_mode(name_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(name_label_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(name_label_, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(name_label_, LV_ALIGN_TOP_MID, 0, 25);

        footer_label_ = lv_label_create(root_);
        lv_obj_set_width(footer_label_, 316);
        lv_label_set_long_mode(footer_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(footer_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(footer_label_, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(footer_label_, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(footer_label_, LV_ALIGN_BOTTOM_MID, 0, -5);

        hal_nfc_clear_last_card();
        hal_nfc_enable();
        refresh_visit_state();
        render();

        if (s_has_pending_visit) {
            const WorkshopVisitResult pending_visit = s_pending_visit;
            s_pending_visit = {};
            s_has_pending_visit = false;
            handle_visit(pending_visit);
        }
    }

    void on_exit() override
    {
        clear_lights();
        root_ = playfield_ = title_label_ = progress_label_ = nullptr;
        name_label_ = footer_label_ = nullptr;
        for (lv_obj_t *&label : cell_labels_) label = nullptr;
    }

    void on_tick() override
    {
        if (light_ticks_ > 0 && --light_ticks_ == 0) clear_lights();
        if (notice_ticks_ > 0) {
            if (--notice_ticks_ == 0) {
                notice_[0] = '\0';
                render();
            }
            return;
        }

        NfcCard card;
        if (!hal_nfc_last_card(&card)) return;

        char payload[64] = {};
        const esp_err_t err = hal_nfc_read_text(payload, sizeof(payload));
        hal_nfc_clear_last_card();
        hal_nfc_field_down(); // keep LED feedback off the energized field
        if (err != ESP_OK) {
            show_notice("TAG READ FAILED");
            return;
        }

        handle_visit(workshop_store_record_payload(payload));
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;

        const size_t count = display_order_.size();
        if (count == 0) return;

        if (ev.button == Button::B || ev.button == Button::Start) {
            const size_t pages = page_count();
            page_ = pages > 0 ? (page_ + 1) % pages : 0;
            selected_ = page_ * kPerPage;
            if (selected_ >= count) selected_ = 0;
            render();
            return;
        }

        const size_t page_start = page_ * kPerPage;
        const size_t page_end = min_size(page_start + kPerPage, count);
        const size_t old = selected_;
        if (ev.button == Button::Left) {
            selected_ = selected_ > 0 ? selected_ - 1 : count - 1;
        } else if (ev.button == Button::Right) {
            selected_ = (selected_ + 1) % count;
        } else if (ev.button == Button::Up
                   && selected_ >= page_start + kColumns) {
            selected_ -= kColumns;
        } else if (ev.button == Button::Down
                   && selected_ + kColumns < page_end) {
            selected_ += kColumns;
        } else if (ev.button == Button::A) {
            show_notice("SCAN THIS WORKSHOP'S NFC TAG");
            return;
        }

        if (selected_ != old) {
            page_ = selected_ / kPerPage;
            render();
        }
    }

private:
    void handle_visit(const WorkshopVisitResult &visit)
    {
        switch (visit.status) {
        case WorkshopVisitStatus::NewVisit:
            refresh_visit_state();
            select_workshop(visit.workshop);
            snprintf(notice_, sizeof(notice_), "COLLECTED: %.4s",
                     visit.workshop ? visit.workshop->code : "WORKSHOP");
            notice_ticks_ = kNoticeTicks;
            start_lights(color_for(workshop_catalog_index(visit.workshop)));
            render();
            break;
        case WorkshopVisitStatus::AlreadyVisited:
            select_workshop(visit.workshop);
            show_notice("ALREADY COLLECTED");
            break;
        case WorkshopVisitStatus::QuestComplete:
            show_notice("QUEST COMPLETE - FIVE COLLECTED!");
            break;
        case WorkshopVisitStatus::InvalidPayload:
            show_notice("UNKNOWN WORKSHOP TAG");
            break;
        case WorkshopVisitStatus::StorageError:
            show_notice("COULDN'T SAVE WORKSHOP");
            break;
        case WorkshopVisitStatus::NotWorkshop:
            show_notice("NOT A WORKSHOP TAG");
            break;
        }
    }

    static size_t min_size(size_t a, size_t b)
    {
        return a < b ? a : b;
    }

    size_t page_count() const
    {
        return (display_order_.size() + kPerPage - 1) / kPerPage;
    }

    static uint32_t color_for(size_t index)
    {
        constexpr size_t count = sizeof(kCollectedColors) / sizeof(kCollectedColors[0]);
        return kCollectedColors[index % count];
    }

    void select_workshop(const WorkshopInfo *workshop)
    {
        const size_t catalog_index = workshop_catalog_index(workshop);
        if (catalog_index >= workshop_catalog_count()) return;
        for (size_t display_index = 0;
             display_index < display_order_.size(); ++display_index) {
            if (display_order_[display_index] != catalog_index) continue;
            selected_ = display_index;
            page_ = selected_ / kPerPage;
            return;
        }
    }

    size_t catalog_index_at(size_t display_index) const
    {
        return display_index < display_order_.size()
            ? display_order_[display_index] : workshop_catalog_count();
    }

    const WorkshopInfo *workshop_at(size_t display_index) const
    {
        return workshop_catalog_at(catalog_index_at(display_index));
    }

    void show_notice(const char *message)
    {
        snprintf(notice_, sizeof(notice_), "%.47s", message ? message : "");
        notice_ticks_ = kNoticeTicks;
        render();
    }

    void start_lights(uint32_t color)
    {
        const uint8_t red = static_cast<uint8_t>((color >> 16) & 0xff);
        const uint8_t green = static_cast<uint8_t>((color >> 8) & 0xff);
        const uint8_t blue = static_cast<uint8_t>(color & 0xff);
        for (size_t i = 0; i < LED_COUNT; ++i) {
            hal_lights_set(i, red, green, blue);
        }
        hal_lights_show();
        light_ticks_ = kLightTicks;
    }

    void clear_lights()
    {
        hal_lights_clear();
        hal_lights_show();
        light_ticks_ = 0;
    }

    void refresh_visit_state()
    {
        for (bool &visited : visited_) visited = false;
        collected_count_ = workshop_store_load(visited_, kCatalogCapacity);

        // Completed workshops stay together at the front without changing
        // the persisted catalog or visit format. The quest goal stays five,
        // but scanning is uncapped so the completed set can grow past it.
        display_order_.clear();
        display_order_.reserve(workshop_catalog_count());
        for (size_t i = 0; i < workshop_catalog_count(); ++i) {
            if (i < kCatalogCapacity && visited_[i]) display_order_.push_back(i);
        }
        for (size_t i = 0; i < workshop_catalog_count(); ++i) {
            if (i >= kCatalogCapacity || !visited_[i]) display_order_.push_back(i);
        }
    }

    static void draw_event(lv_event_t *event)
    {
        auto *app = static_cast<WorkshopsApp *>(lv_event_get_user_data(event));
        if (app) app->draw_hex_grid(lv_event_get_layer(event));
    }

    static void draw_hex(lv_layer_t *layer, int x, int y,
                         int width, int height, uint32_t color)
    {
        const int point = width / 5;

        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_opa = LV_OPA_COVER;
        rect.bg_color = lv_color_hex(color);
        lv_area_t middle = {
            x + point, y,
            x + width - point - 1, y + height - 1,
        };
        lv_draw_rect(layer, &rect, &middle);

        lv_draw_triangle_dsc_t triangle;
        lv_draw_triangle_dsc_init(&triangle);
        triangle.color = lv_color_hex(color);
        triangle.opa = LV_OPA_COVER;
        triangle.p[0].x = x + point;
        triangle.p[0].y = y;
        triangle.p[1].x = x;
        triangle.p[1].y = y + height / 2;
        triangle.p[2].x = x + point;
        triangle.p[2].y = y + height - 1;
        lv_draw_triangle(layer, &triangle);

        triangle.p[0].x = x + width - point - 1;
        triangle.p[0].y = y;
        triangle.p[1].x = x + width - 1;
        triangle.p[1].y = y + height / 2;
        triangle.p[2].x = x + width - point - 1;
        triangle.p[2].y = y + height - 1;
        lv_draw_triangle(layer, &triangle);
    }

    void draw_hex_grid(lv_layer_t *layer)
    {
        const size_t page_start = page_ * kPerPage;
        for (size_t slot = 0; slot < kPerPage; ++slot) {
            const size_t display_index = page_start + slot;
            if (display_index >= display_order_.size()) continue;
            const size_t catalog_index = catalog_index_at(display_index);

            const int column = static_cast<int>(slot % kColumns);
            const int row = static_cast<int>(slot / kColumns);
            const int x = kHexX + column * kHexPitchX;
            const int y = kHexY + row * kHexPitchY
                + (column % 2) * kColumnStagger;
            const bool visited = catalog_index < kCatalogCapacity
                && visited_[catalog_index];
            const uint32_t fill = visited ? color_for(catalog_index) : 0x162332;
            const uint32_t border = display_index == selected_ ? 0xffffff
                : visited ? color_for(catalog_index) : 0x34495e;

            draw_hex(layer, x, y, kHexWidth, kHexHeight, border);
            draw_hex(layer, x + 2, y + 2,
                     kHexWidth - 4, kHexHeight - 4, fill);
        }
    }

    void render()
    {
        lv_label_set_text(title_label_, "Workshop Quest");
        lv_label_set_text_fmt(progress_label_, "%u/%u",
            static_cast<unsigned>(collected_count_),
            static_cast<unsigned>(kWorkshopQuestGoal));
        lv_obj_set_style_text_color(progress_label_,
            lv_color_hex(collected_count_ >= kWorkshopQuestGoal
                ? 0xffd166 : BadgeTheme::ACCENT_DETAIL), 0);

        const WorkshopInfo *selected = workshop_at(selected_);
        lv_label_set_text(name_label_, selected ? selected->name : "Workshop");

        const size_t page_start = page_ * kPerPage;
        for (size_t slot = 0; slot < kPerPage; ++slot) {
            const size_t display_index = page_start + slot;
            const size_t catalog_index = catalog_index_at(display_index);
            const WorkshopInfo *workshop = workshop_at(display_index);
            if (!workshop) {
                lv_obj_add_flag(cell_labels_[slot], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_clear_flag(cell_labels_[slot], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(cell_labels_[slot], workshop->code);
            const bool visited = catalog_index < kCatalogCapacity
                && visited_[catalog_index];
            lv_obj_set_style_text_color(cell_labels_[slot],
                lv_color_hex(visited ? 0xffffff : BadgeTheme::TEXT_MUTED), 0);
        }

        if (notice_[0]) {
            lv_label_set_text(footer_label_, notice_);
            lv_obj_set_style_text_color(footer_label_,
                lv_color_hex(0xffd166), 0);
        } else {
            lv_label_set_text_fmt(footer_label_,
                "SCAN TAG   D-PAD BROWSE   B PAGE %u/%u",
                static_cast<unsigned>(page_ + 1),
                static_cast<unsigned>(page_count()));
            lv_obj_set_style_text_color(footer_label_,
                lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        }

        lv_obj_invalidate(playfield_);
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *playfield_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *progress_label_ = nullptr;
    lv_obj_t *name_label_ = nullptr;
    lv_obj_t *footer_label_ = nullptr;
    lv_obj_t *cell_labels_[kPerPage] = {};
    size_t selected_ = 0;
    size_t page_ = 0;
    int notice_ticks_ = 0;
    int light_ticks_ = 0;
    size_t collected_count_ = 0;
    bool visited_[kCatalogCapacity] = {};
    std::vector<size_t> display_order_;
    char notice_[48] = {};
};

WorkshopsApp s_instance;

} // namespace

App *app_workshops_instance() { return &s_instance; }

void app_workshops_prepare_visit(const WorkshopVisitResult &visit)
{
    s_pending_visit = visit;
    s_has_pending_visit = true;
}
