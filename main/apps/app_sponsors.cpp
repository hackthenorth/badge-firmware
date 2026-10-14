#include "app_sponsors.h"

#include "app.h"
#include "app_registry.h"
#include "app_scanner.h"
#include "hal_lights.h"
#include "hal_nfc.h"
#include "sponsor_catalog.h"
#include "sponsor_layout.h"
#include "sponsor_store.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>

namespace {

constexpr uint32_t kVisitedBg = 0x173f32;
constexpr uint32_t kVisitedText = 0x76e6a9;
constexpr uint32_t kVisitedBoothBg = 0x6d3fb2;
constexpr uint32_t kVisitedBoothText = 0xf2e9ff;
// Light blue keeps Floor 1 distinct; fresh mint green keeps Floor 2 lively.
// Both preserve strong contrast with the dark map labels.
constexpr uint32_t kFloorOneFill = 0xbfe7f5;
constexpr uint32_t kFloorTwoFill = 0x96d8a5;
constexpr uint32_t kFloorText = 0x263746;
constexpr uint32_t kFloorWall = 0x617585;
constexpr uint32_t kMapSelection = 0xff7a00;
constexpr int kNoticeTicks = 100; // 2 seconds at the registry's 50 Hz tick
constexpr int kDiscoveryTicks = 125; // 2.5 seconds
constexpr int kDiscoveryFlashOnTicks = 10; // 200 ms
constexpr int kDiscoveryFlashCycleTicks = 20; // 400 ms
constexpr int kDiscoveryFlashCount = 2;
constexpr int kListRows = 6;
constexpr size_t kMapCellsPerTick = 16;
constexpr uint32_t kIdRevealHoldMs = 600;
constexpr size_t kAssignmentCacheCapacity = kSponsorMapMaxTables;

// Floor guides from the original two-floor map. Region B's four booths fit
// inside the Floor 1 outline; the other regions are on Floor 2.
// lv_line keeps these point arrays by reference.
constexpr lv_point_precise_t kFloorOneOutline[] = {
    {0, 0}, {0, 100}, {79, 100}, {79, 73},
};
constexpr lv_point_precise_t kFloorOneUpperWall[] = {
    {0, 0}, {0, 41},
};
constexpr lv_point_precise_t kStairs[] = {
    {0, 30}, {4, 30}, {4, 22}, {8, 22},
    {8, 14}, {12, 14}, {12, 6},
};
constexpr lv_point_precise_t kRightWingOutline[] = {
    {0, 0}, {0, 34}, {107, 34},
};
constexpr lv_point_precise_t kBayArrow[] = {
    {30, 18}, {6, 18}, {6, 0}, {0, 6}, {6, 0}, {12, 6},
};

SponsorTableId s_pending_table = 0;
bool s_pending_new = false;

void sponsor_initial(const char *name, char out[2])
{
    out[0] = '?';
    out[1] = '\0';
    for (size_t i = 0; name && name[i]; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (std::isalnum(c)) {
            out[0] = static_cast<char>(std::toupper(c));
            return;
        }
    }
}

void style_label(lv_obj_t *label, uint32_t color, const lv_font_t *font)
{
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
}

extern "C" const lv_image_dsc_t icon_sponsors;
extern "C" const lv_image_dsc_t scanner_sticker;

class SponsorsApp final : public App {
public:
    const char *name() const override { return "Sponsors"; }
    const char *icon() const override { return "SPN"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_sponsors; }
    const char *slug() const override { return "sponsors"; }
    AppAccess access() const override { return AppAccess::Default; }
    // Entry always reboots first (registry): the map + NFC session wants a
    // fresh heap, and exit reboots back for the same reason.
    bool uses_nfc() const override { return true; }
    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        data_.reset(new (std::nothrow) SessionData{});
        if (!data_) {
            lv_obj_t *oops = lv_label_create(root);
            lv_label_set_text(oops, "Not enough memory\nHOME for Apps");
            style_label(oops, BadgeTheme::TEXT, &lv_font_montserrat_16);
            lv_obj_center(oops);
            return;
        }
        mode_ = Mode::Map;
        show_ids_ = false;
        list_offset_ = 0;
        notice_ticks_ = 0;
        notice_[0] = '\0';
        discovery_ticks_ = 0;
        discovery_elapsed_ticks_ = 0;
        discovery_table_id_ = 0;
        discovery_leds_on_ = false;
        sponsor_layout_load(&layout_);
        reset_table_data();

        if (s_pending_table != 0) {
            if (s_pending_table <= layout_.table_count) {
                selected_table_index_ = static_cast<size_t>(s_pending_table - 1);
            }
            set_visit_notice(s_pending_table, s_pending_new);
        } else if (selected_table_index_ >= layout_.table_count) {
            selected_table_index_ = 0;
        }
        s_pending_table = 0;
        s_pending_new = false;

        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        hal_nfc_clear_last_card();
        nfc_started_ = false;
        render();
    }

    void on_exit() override
    {
        set_discovery_leds(false);
        clear_ui_refs();
        data_.reset();
        root_ = nullptr;
        map_build_index_ = layout_.table_count;
        nfc_started_ = false;
        start_held_ = false;
        show_ids_ = false;
    }

    void on_tick() override
    {
        if (!data_) return;
        if (!nfc_started_) {
            // NFC power-up can perform I2C work. Defer it until after the
            // floor plan's first frame has been handed back to LVGL.
            hal_nfc_enable();
            nfc_started_ = true;
            return;
        }

        if (start_held_ && mode_ == Mode::Map && !show_ids_
            && lv_tick_elaps(start_pressed_at_ms_) >= kIdRevealHoldMs) {
            show_ids_ = true;
            refresh_all_map_cells();
        }

        if (discovery_ticks_ > 0) {
            tick_discovery();
            return;
        }

        if (mode_ == Mode::Map) build_next_map_cells();

        if (notice_ticks_ > 0 && --notice_ticks_ == 0) {
            notice_[0] = '\0';
            refresh_header();
        }

        NfcCard card;
        if (!hal_nfc_last_card(&card)) return;

        char payload[64] = {};
        const esp_err_t err = hal_nfc_read_text(payload, sizeof(payload));
        hal_nfc_clear_last_card();
        hal_nfc_field_down(); // keep LED feedback off the energized field
        if (err != ESP_OK) {
            set_notice("TAG READ FAILED");
            refresh_header();
            return;
        }

        const SponsorVisitResult visit = sponsor_store_record_payload(payload);
        switch (visit.status) {
        case SponsorVisitStatus::NewVisit:
        case SponsorVisitStatus::AlreadyVisited:
            if (visit.table_id <= layout_.table_count) {
                selected_table_index_ = static_cast<size_t>(visit.table_id - 1);
            }
            mode_ = Mode::Map;
            show_ids_ = false;
            sponsor_layout_load(&layout_);
            reset_table_data();
            set_visit_notice(visit.table_id,
                             visit.status == SponsorVisitStatus::NewVisit);
            render();
            return;
        case SponsorVisitStatus::InvalidPayload:
            set_notice("INVALID SPONSOR TAG");
            refresh_header();
            return;
        case SponsorVisitStatus::StorageError:
            set_notice("SAVE FAILED");
            refresh_header();
            return;
        case SponsorVisitStatus::NotSponsor:
            app_scanner_prepare_payload(payload);
            app_registry_launch(app_scanner_instance());
            return;
        }
    }

    void on_button(ButtonEvent ev) override
    {
        if (!data_) return;
        if (discovery_ticks_ > 0) {
            if (ev.kind == ButtonEventKind::Pressed
                && (ev.button == Button::A || ev.button == Button::B
                    || ev.button == Button::Start)) {
                dismiss_discovery();
            }
            return;
        }

        if (ev.button == Button::Start && mode_ == Mode::Map) {
            // A tap opens the list; a hold previews table IDs for staff.
            // Decide on release so the hidden shortcut never opens the list.
            if (ev.kind == ButtonEventKind::Pressed) {
                if (!start_held_) start_pressed_at_ms_ = lv_tick_get();
                start_held_ = true;
            } else if (start_held_) {
                start_held_ = false;
                if (show_ids_
                    || lv_tick_elaps(start_pressed_at_ms_) >= kIdRevealHoldMs) {
                    show_ids_ = false;
                    refresh_all_map_cells();
                } else {
                    mode_ = Mode::List;
                    render();
                }
            }
            return;
        }
        if (ev.kind != ButtonEventKind::Pressed) return;

        // Start toggles the list/map. HOME is handled by the registry.
        if (ev.button == Button::Start) {
            mode_ = mode_ == Mode::List ? Mode::Map : Mode::List;
            render();
            return;
        }

        if (mode_ == Mode::Detail) {
            if (ev.button == Button::A || ev.button == Button::B) {
                mode_ = detail_return_mode_;
                render();
            }
        } else if (mode_ == Mode::Info) {
            if (ev.button == Button::B) show_map();
        } else if (ev.button == Button::A) {
            open_detail();
        } else if (ev.button == Button::B) {
            if (mode_ == Mode::List) {
                show_map();
            } else {
                mode_ = Mode::Info;
                render();
            }
        } else if (mode_ == Mode::Map) {
            move_map(ev.button);
        } else if (mode_ == Mode::List) {
            move_list(ev.button);
        }
    }

private:
    // The launcher needs only app metadata. Map and assignment caches exist
    // during a visit and are released before returning to any other app.
    struct SessionData {
        lv_obj_t *map_cells[kSponsorMapMaxTables];
        lv_obj_t *map_labels[kSponsorMapMaxTables];
        SponsorTableState table_states[kSponsorMapMaxTables];
        bool table_state_loaded[kSponsorMapMaxTables];
        SponsorStoredAssignment assignments[kAssignmentCacheCapacity];
    };

    enum class Mode : uint8_t { Map, Info, List, Detail };

    void reset_table_data()
    {
        for (size_t i = 0; i < kSponsorMapMaxTables; ++i) {
            data_->table_states[i] = {};
            data_->table_state_loaded[i] = false;
        }
        visited_count_ = 0;
        map_visited_count_ = 0;
        visit_total_loaded_ = false;
        table_states_complete_ = false;
        map_build_index_ = layout_.table_count;
        assignments_loaded_ = false;
        assignment_count_ = 0;
        unknown_assignment_count_ = 0;
    }

    void load_assignments()
    {
        if (assignments_loaded_) return;
        // Lists need only the stored assignments, so keep this directory scan
        // out of the map's first frame and pay for it only if the list opens.
        assignment_count_ = sponsor_store_list_assignments(
            data_->assignments, kAssignmentCacheCapacity);
        unknown_assignment_count_ = 0;
        for (size_t i = 0; i < assignment_count_; ++i) {
            if (!data_->assignments[i].sponsor) ++unknown_assignment_count_;
        }
        // Overflow tables count toward the quest but deliberately have no
        // invented position on the configured map.
        visited_count_ = sponsor_store_count();
        visit_total_loaded_ = true;
        assignments_loaded_ = true;
    }

    void ensure_table_state(size_t index)
    {
        if (index >= layout_.table_count || data_->table_state_loaded[index]) return;
        SponsorTableState state = {};
        sponsor_store_get_table_state(static_cast<SponsorTableId>(index + 1),
                                      &state);
        data_->table_states[index] = state;
        data_->table_state_loaded[index] = true;
        if (state.visited) ++map_visited_count_;
    }

    SponsorTableId table_for_sponsor(const SponsorInfo *sponsor) const
    {
        if (!sponsor) return 0;
        for (size_t i = 0; i < assignment_count_; ++i) {
            if (data_->assignments[i].sponsor == sponsor) {
                return data_->assignments[i].table_id;
            }
        }
        return 0;
    }

    const SponsorStoredAssignment *unknown_assignment_at(size_t index) const
    {
        for (size_t i = 0; i < assignment_count_; ++i) {
            if (data_->assignments[i].sponsor) continue;
            if (index == 0) return &data_->assignments[i];
            --index;
        }
        return nullptr;
    }

    size_t list_entry_count() const
    {
        return sponsor_catalog_count() + unknown_assignment_count_;
    }

    void clear_ui_refs()
    {
        title_label_ = nullptr;
        progress_label_ = nullptr;
        discovery_scrim_ = nullptr;
        discovery_panel_ = nullptr;
        if (!data_) return;
        for (size_t i = 0; i < kSponsorMapMaxTables; ++i) {
            data_->map_cells[i] = nullptr;
            data_->map_labels[i] = nullptr;
        }
    }

    void render()
    {
        if (!root_) return;
        if (mode_ == Mode::List) load_assignments();
        start_held_ = false;
        show_ids_ = false;
        map_build_index_ = layout_.table_count;
        clear_ui_refs();
        lv_obj_clean(root_);
        render_header();
        switch (mode_) {
        case Mode::Map:    render_map(); break;
        case Mode::Info:   render_info(); break;
        case Mode::List:   render_list(); break;
        case Mode::Detail: render_detail(); break;
        }
        if (discovery_ticks_ > 0) render_discovery();
    }

    void render_header()
    {
        title_label_ = lv_label_create(root_);
        lv_obj_set_width(title_label_, 205);
        lv_label_set_long_mode(title_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(title_label_, 10, 5);

        progress_label_ = lv_label_create(root_);
        style_label(progress_label_, BadgeTheme::TEXT_SOFT,
                    &lv_font_montserrat_14);
        refresh_header();

        lv_obj_t *line = lv_obj_create(root_);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, 302, 1);
        lv_obj_set_pos(line, 9, 31);
        lv_obj_set_style_bg_color(line, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    }

    void refresh_header()
    {
        if (title_label_) {
            style_label(title_label_,
                notice_ticks_ > 0 ? BadgeTheme::ACCENT_DETAIL : BadgeTheme::TEXT,
                notice_ticks_ > 0 ? &lv_font_montserrat_14
                                  : &lv_font_montserrat_18);
            // Style first: LVGL's DOT long-mode caches the text fit using the
            // current font, so changing from its default font afterward can
            // truncate the very first frame.
            lv_label_set_text(title_label_, notice_ticks_ > 0 ? notice_ :
                mode_ == Mode::Detail ? "Sponsor detail" :
                mode_ == Mode::Info ? "Sponsor taps" : "Sponsors");
        }
        if (progress_label_) {
            if (!visit_total_loaded_) {
                lv_label_set_text_fmt(progress_label_, "--/%u",
                    static_cast<unsigned>(layout_.table_count));
                lv_obj_align(progress_label_, LV_ALIGN_TOP_RIGHT, -9, 8);
                return;
            }
            const unsigned percent = static_cast<unsigned>(
                layout_.table_count == 0 ? 0
                    : (visited_count_ * 100) / layout_.table_count);
            lv_label_set_text_fmt(progress_label_, "%u/%u  %u%%",
                static_cast<unsigned>(visited_count_),
                static_cast<unsigned>(layout_.table_count), percent);
            lv_obj_align(progress_label_, LV_ALIGN_TOP_RIGHT, -9, 8);
        }
    }

    template<size_t N>
    void make_floorplan_line(const lv_point_precise_t (&points)[N],
                             int x, int y,
                             uint32_t color = kFloorWall)
    {
        lv_obj_t *line = lv_line_create(root_);
        lv_line_set_points(line, points, N);
        lv_obj_set_pos(line, x, y + kSponsorMapYOffset);
        lv_obj_set_style_line_width(line, 1, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
        lv_obj_set_style_line_rounded(line, false, 0);
    }

    void make_floor_label(const char *text, int x, int y)
    {
        lv_obj_t *label = lv_label_create(root_);
        lv_label_set_text(label, text);
        style_label(label, kFloorText, &lv_font_montserrat_14);
        lv_obj_set_pos(label, x, y + kSponsorMapYOffset);
    }

    void make_floor_fill(int x, int y, int width, int height, uint32_t color)
    {
        lv_obj_t *fill = lv_obj_create(root_);
        lv_obj_remove_style_all(fill);
        lv_obj_set_pos(fill, x, y + kSponsorMapYOffset);
        lv_obj_set_size(fill, width, height);
        lv_obj_set_style_bg_color(fill, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(fill, LV_OBJ_FLAG_CLICKABLE);
    }

    void render_map()
    {
        // Floor 2 surrounds the Floor 1 room. Keep the upper-right cutout
        // outside the floor plan, and draw booths above both solid fills.
        make_floor_fill(0, 47, 320, 154, kFloorTwoFill);
        make_floor_fill(54, 47, 80, 101, kFloorOneFill);
        make_floor_fill(197, 47, 107, 34, BadgeTheme::BACKGROUND);
        make_floorplan_line(kFloorOneOutline, 54, 47);
        make_floorplan_line(kFloorOneUpperWall, 133, 47);
        make_floorplan_line(kStairs, 135, 88, BadgeTheme::ACCENT_DETAIL);
        make_floorplan_line(kRightWingOutline, 197, 47);
        make_floor_label("Floor 1", 68, 62);
        make_floor_label("Floor 2", 68, 154);

        lv_obj_t *bay = lv_label_create(root_);
        lv_label_set_text(bay, "Extended\nSponsor Bay");
        style_label(bay, kFloorText, &lv_font_montserrat_12);
        lv_obj_set_width(bay, 105);
        lv_obj_set_style_text_align(bay, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(bay, 36, 172 + kSponsorMapYOffset);

        lv_obj_t *arrow = lv_line_create(root_);
        lv_line_set_points(arrow, kBayArrow, sizeof(kBayArrow) / sizeof(kBayArrow[0]));
        lv_obj_set_pos(arrow, 18, 176 + kSponsorMapYOffset);
        lv_obj_set_style_line_width(arrow, 2, 0);
        lv_obj_set_style_line_color(arrow, lv_color_hex(kFloorText), 0);
        lv_obj_set_style_line_rounded(arrow, true, 0);

        make_footer("A SPONSOR  B INFO  START LIST\nHOME APPS",
                    &lv_font_montserrat_12);
        // Return from on_enter() with the useful floor plan visible. Booth
        // State reads and LVGL object construction are spread across four
        // quick batches so the 64-table map appears fast without blocking the
        // app-open frame on every object and filesystem read at once.
        map_build_index_ = 0;
    }

    void create_map_cell(size_t index)
    {
        SponsorBoothPosition position;
        if (!sponsor_layout_booth_position(layout_, index, &position)) return;

        ensure_table_state(index);
        data_->map_cells[index] = lv_obj_create(root_);
        lv_obj_remove_style_all(data_->map_cells[index]);
        lv_obj_set_size(data_->map_cells[index], position.size, position.size);
        lv_obj_set_pos(data_->map_cells[index], position.x, position.y);
        lv_obj_set_style_radius(data_->map_cells[index], position.size >= 8 ? 3 : 1, 0);
        lv_obj_set_style_bg_opa(data_->map_cells[index], LV_OPA_COVER, 0);
        lv_obj_clear_flag(data_->map_cells[index], LV_OBJ_FLAG_SCROLLABLE);

        if (position.size >= 10) {
            data_->map_labels[index] = lv_label_create(data_->map_cells[index]);
            style_label(data_->map_labels[index], BadgeTheme::TEXT_MUTED,
                        &lv_font_montserrat_12);
            lv_obj_center(data_->map_labels[index]);
        }
        refresh_map_cell(index);
    }

    void build_next_map_cells()
    {
        if (!root_ || map_build_index_ >= layout_.table_count) return;
        const size_t end = map_build_index_ + kMapCellsPerTick < layout_.table_count
            ? map_build_index_ + kMapCellsPerTick : layout_.table_count;
        while (map_build_index_ < end) create_map_cell(map_build_index_++);
        if (map_build_index_ == layout_.table_count && !table_states_complete_) {
            table_states_complete_ = true;
            visited_count_ = map_visited_count_ + sponsor_store_count_after(
                static_cast<SponsorTableId>(layout_.table_count));
            visit_total_loaded_ = true;
            refresh_header();
        }
    }

    void render_info()
    {
        lv_obj_t *sticker = lv_image_create(root_);
        lv_image_set_src(sticker, &scanner_sticker);
        lv_obj_set_pos(sticker, 12, 50);

        lv_obj_t *where = lv_label_create(root_);
        lv_obj_set_width(where, 190);
        lv_label_set_long_mode(where, LV_LABEL_LONG_WRAP);
        lv_label_set_text(where, "Look for this tap\nsticker on sponsor\ntables.");
        style_label(where, BadgeTheme::TEXT_SOFT, &lv_font_montserrat_14);
        lv_obj_set_pos(where, 118, 48);

        lv_obj_t *how = lv_label_create(root_);
        lv_obj_set_width(how, 190);
        lv_label_set_long_mode(how, LV_LABEL_LONG_WRAP);
        lv_label_set_text(how, "Tap your badge here\nto track who you've met.");
        style_label(how, BadgeTheme::ACCENT_DETAIL, &lv_font_montserrat_14);
        lv_obj_set_pos(how, 118, 108);

        lv_obj_t *why = lv_label_create(root_);
        lv_obj_set_width(why, 296);
        lv_label_set_long_mode(why, LV_LABEL_LONG_WRAP);
        lv_label_set_text(why, "Learn more about each sponsor\n"
                               "and help complete the sponsor quest!");
        style_label(why, BadgeTheme::TEXT_SOFT, &lv_font_montserrat_14);
        lv_obj_set_pos(why, 12, 162);

        make_footer("B MAP  START LIST  HOME APPS", &lv_font_montserrat_12);
    }

    void refresh_map_cell(size_t index)
    {
        if (index >= layout_.table_count || !data_->map_cells[index]) return;

        const SponsorTableState &state = data_->table_states[index];
        const bool selected = index == selected_table_index_;
        lv_obj_set_style_bg_color(data_->map_cells[index],
            lv_color_hex(state.visited ? kVisitedBoothBg : BadgeTheme::PANEL), 0);
        lv_obj_set_style_border_width(data_->map_cells[index], selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(data_->map_cells[index],
            lv_color_hex(selected ? kMapSelection
                                  : state.visited ? kVisitedBoothText
                                                  : BadgeTheme::BORDER), 0);

        if (!data_->map_labels[index]) return;

        // Leave room for every unsigned value, including its terminator.
        char text[11];
        if (show_ids_) {
            snprintf(text, sizeof(text), "%u", static_cast<unsigned>(index + 1));
        } else if (state.sponsor) {
            sponsor_initial(state.sponsor->name, text);
        } else if (state.slug[0]) {
            sponsor_initial(state.slug, text);
        } else {
            snprintf(text, sizeof(text), "?");
        }
        lv_label_set_text(data_->map_labels[index], text);
        lv_obj_center(data_->map_labels[index]);
        lv_obj_set_style_text_color(data_->map_labels[index],
            lv_color_hex(state.visited ? kVisitedBoothText
                                       : BadgeTheme::TEXT_MUTED), 0);
    }

    void refresh_all_map_cells()
    {
        for (size_t i = 0; i < layout_.table_count; ++i) refresh_map_cell(i);
    }

    void render_list()
    {
        const size_t entry_count = list_entry_count();
        if (entry_count > 0 && selected_sponsor_index_ >= entry_count) {
            selected_sponsor_index_ = entry_count - 1;
        }
        normalize_list_offset();
        for (int row = 0; row < kListRows; ++row) {
            const size_t index = list_offset_ + static_cast<size_t>(row);
            if (index >= entry_count) break;

            const SponsorInfo *sponsor = nullptr;
            const SponsorStoredAssignment *unknown = nullptr;
            SponsorTableId table_id = 0;
            const char *display_name = nullptr;
            if (index < sponsor_catalog_count()) {
                sponsor = sponsor_catalog_at(index);
                table_id = table_for_sponsor(sponsor);
                display_name = sponsor ? sponsor->name : "???";
            } else {
                unknown = unknown_assignment_at(index - sponsor_catalog_count());
                if (!unknown) break;
                table_id = unknown->table_id;
                display_name = unknown->slug;
            }

            const bool selected = index == selected_sponsor_index_;
            lv_obj_t *panel = lv_obj_create(root_);
            lv_obj_remove_style_all(panel);
            lv_obj_set_size(panel, 302, 27);
            lv_obj_set_pos(panel, 9, 35 + row * 29);
            lv_obj_set_style_radius(panel, 4, 0);
            lv_obj_set_style_bg_color(panel,
                lv_color_hex(selected ? BadgeTheme::SURFACE
                                      : BadgeTheme::BACKGROUND), 0);
            lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
            if (selected) {
                lv_obj_set_style_border_width(panel, 1, 0);
                lv_obj_set_style_border_color(panel,
                    lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
            }
            lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

            char table_text[8] = "??";
            if (table_id) snprintf(table_text, sizeof(table_text), "%02u",
                                   static_cast<unsigned>(table_id));
            lv_obj_t *label = lv_label_create(panel);
            lv_obj_set_width(label, 284);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_label_set_text_fmt(label, "%s  %s  %s",
                table_id ? "+" : "-", table_text, display_name);
            style_label(label, table_id ? kVisitedText : BadgeTheme::TEXT_SOFT,
                        &lv_font_montserrat_14);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
        }

        make_footer("U/D BROWSE  A SPONSOR\nB/START MAP  HOME APPS",
                    &lv_font_montserrat_12);
    }

    void open_detail()
    {
        detail_return_mode_ = mode_;
        if (mode_ == Mode::Map) {
            ensure_table_state(selected_table_index_);
            detail_table_id_ =
                static_cast<SponsorTableId>(selected_table_index_ + 1);
            detail_sponsor_ = data_->table_states[selected_table_index_].sponsor;
            snprintf(detail_slug_, sizeof(detail_slug_), "%s",
                     data_->table_states[selected_table_index_].slug);
        } else {
            if (selected_sponsor_index_ < sponsor_catalog_count()) {
                detail_sponsor_ = sponsor_catalog_at(selected_sponsor_index_);
                detail_table_id_ = table_for_sponsor(detail_sponsor_);
                snprintf(detail_slug_, sizeof(detail_slug_), "%s",
                         detail_sponsor_ ? detail_sponsor_->slug : "");
            } else {
                const SponsorStoredAssignment *unknown = unknown_assignment_at(
                    selected_sponsor_index_ - sponsor_catalog_count());
                detail_sponsor_ = nullptr;
                detail_table_id_ = unknown ? unknown->table_id : 0;
                snprintf(detail_slug_, sizeof(detail_slug_), "%s",
                         unknown ? unknown->slug : "");
            }
        }
        mode_ = Mode::Detail;
        render();
    }

    void render_detail()
    {
        const bool visited = detail_table_id_ != 0
            && sponsor_store_is_visited(detail_table_id_);

        lv_obj_t *body = lv_obj_create(root_);
        lv_obj_remove_style_all(body);
        lv_obj_set_size(body, 300, 180);
        lv_obj_set_pos(body, 10, 35);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(body, 5, 0);
        lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

        char table_text[8] = "??";
        if (detail_table_id_) {
            snprintf(table_text, sizeof(table_text), "%02u",
                     static_cast<unsigned>(detail_table_id_));
        }
        lv_obj_t *status = lv_label_create(body);
        lv_label_set_text_fmt(status, "TABLE %s  -  %s", table_text,
                             visited ? "VISITED" : "NOT VISITED");
        style_label(status, visited ? kVisitedText : BadgeTheme::TEXT_MUTED,
                    &lv_font_montserrat_14);

        lv_obj_t *name = lv_label_create(body);
        lv_obj_set_width(name, 296);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_label_set_text(name, detail_sponsor_ ? detail_sponsor_->name
                                               : detail_slug_[0]
                                                   ? detail_slug_ : "???");
        style_label(name, BadgeTheme::TEXT, &lv_font_montserrat_22);

        lv_obj_t *description = lv_label_create(body);
        lv_obj_set_width(description, 296);
        lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
        lv_label_set_text(description, detail_sponsor_
            ? detail_sponsor_->description
            : detail_slug_[0]
                ? "This sponsor is not in the badge directory yet. Showing the slug saved from its table tag."
                : "Sponsor not known yet. Scan this table's sponsor tag to identify it.");
        style_label(description, BadgeTheme::TEXT_SOFT,
                    &lv_font_montserrat_14);

        if (detail_sponsor_) {
            lv_obj_t *link = lv_label_create(body);
            lv_obj_set_width(link, 296);
            lv_label_set_long_mode(link, LV_LABEL_LONG_WRAP);
            lv_label_set_text(link, detail_sponsor_->link);
            style_label(link, BadgeTheme::ACCENT_DETAIL,
                        &lv_font_montserrat_14);
        }

        make_footer(detail_return_mode_ == Mode::List
            ? "A/B/START LIST  HOME APPS"
            : "A/B MAP  START LIST  HOME APPS", &lv_font_montserrat_12);
    }

    void make_footer(const char *text,
                     const lv_font_t *font = &lv_font_montserrat_14)
    {
        lv_obj_t *footer = lv_label_create(root_);
        lv_label_set_text(footer, text);
        style_label(footer, BadgeTheme::TEXT_DIM, font);
        lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    void show_map()
    {
        mode_ = Mode::Map;
        show_ids_ = false;
        render();
    }

    void move_map(Button button)
    {
        const size_t old = selected_table_index_;
        SponsorBoothPosition current;
        if (!sponsor_layout_booth_position(layout_, old, &current)) return;

        int best_score = INT_MAX;
        for (size_t candidate = 0; candidate < layout_.table_count; ++candidate) {
            if (candidate == old) continue;

            SponsorBoothPosition next;
            if (!sponsor_layout_booth_position(layout_, candidate, &next)) continue;
            const int dx = static_cast<int>(next.x) - current.x;
            const int dy = static_cast<int>(next.y) - current.y;
            int forward = 0;
            int sideways = 0;
            if (button == Button::Up && dy < 0) {
                forward = -dy;
                sideways = std::abs(dx);
            } else if (button == Button::Down && dy > 0) {
                forward = dy;
                sideways = std::abs(dx);
            } else if (button == Button::Left && dx < 0) {
                forward = -dx;
                sideways = std::abs(dy);
            } else if (button == Button::Right && dx > 0) {
                forward = dx;
                sideways = std::abs(dy);
            } else {
                continue;
            }

            const int score = forward + sideways * 4;
            if (score < best_score) {
                best_score = score;
                selected_table_index_ = candidate;
            }
        }
        if (selected_table_index_ != old) {
            refresh_map_cell(old);
            refresh_map_cell(selected_table_index_);
        }
    }

    void move_list(Button button)
    {
        const size_t old = selected_sponsor_index_;
        if ((button == Button::Up || button == Button::Left)
            && selected_sponsor_index_ > 0) {
            --selected_sponsor_index_;
        } else if ((button == Button::Down || button == Button::Right)
                   && selected_sponsor_index_ + 1 < list_entry_count()) {
            ++selected_sponsor_index_;
        }
        if (selected_sponsor_index_ != old) render();
    }

    void normalize_list_offset()
    {
        if (selected_sponsor_index_ < list_offset_) {
            list_offset_ = selected_sponsor_index_;
        }
        if (selected_sponsor_index_ >= list_offset_ + kListRows) {
            list_offset_ = selected_sponsor_index_ - kListRows + 1;
        }
    }

    void set_visit_notice(SponsorTableId table_id, bool is_new)
    {
        if (is_new) {
            begin_discovery(table_id);
            return;
        }
        snprintf(notice_, sizeof(notice_),
                 "TABLE %u VISITED",
                 static_cast<unsigned>(table_id));
        notice_ticks_ = kNoticeTicks;
    }

    void begin_discovery(SponsorTableId table_id)
    {
        discovery_table_id_ = table_id;
        discovery_ticks_ = kDiscoveryTicks;
        discovery_elapsed_ticks_ = 0;
        notice_ticks_ = 0;
        notice_[0] = '\0';
        set_discovery_leds(true);
    }

    void render_discovery()
    {
        if (!root_ || discovery_table_id_ == 0) return;

        discovery_scrim_ = lv_obj_create(root_);
        lv_obj_remove_style_all(discovery_scrim_);
        lv_obj_set_size(discovery_scrim_, 320, 240);
        lv_obj_set_pos(discovery_scrim_, 0, 0);
        lv_obj_set_style_bg_color(discovery_scrim_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(discovery_scrim_, LV_OPA_70, 0);
        lv_obj_clear_flag(discovery_scrim_, LV_OBJ_FLAG_SCROLLABLE);

        discovery_panel_ = lv_obj_create(discovery_scrim_);
        lv_obj_remove_style_all(discovery_panel_);
        lv_obj_set_size(discovery_panel_, 286, 164);
        lv_obj_set_style_bg_color(discovery_panel_, lv_color_hex(kVisitedBg), 0);
        lv_obj_set_style_bg_opa(discovery_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(discovery_panel_, 3, 0);
        lv_obj_set_style_border_color(discovery_panel_,
                                      lv_color_hex(kVisitedText), 0);
        lv_obj_set_style_radius(discovery_panel_, 14, 0);
        lv_obj_clear_flag(discovery_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_center(discovery_panel_);

        lv_obj_t *eyebrow = lv_label_create(discovery_panel_);
        lv_label_set_text(eyebrow, "SPONSOR DISCOVERED");
        style_label(eyebrow, kVisitedText, &lv_font_montserrat_14);
        lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t *message = lv_label_create(discovery_panel_);
        lv_obj_set_width(message, 258);
        lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(message, "You visited a new sponsor!");
        style_label(message, BadgeTheme::TEXT, &lv_font_montserrat_18);
        lv_obj_align(message, LV_ALIGN_TOP_MID, 0, 36);

        SponsorTableState state;
        sponsor_store_get_table_state(discovery_table_id_, &state);
        const char *sponsor_name = state.sponsor ? state.sponsor->name
                                                 : state.slug[0]
                                                     ? state.slug : "???";
        lv_obj_t *name = lv_label_create(discovery_panel_);
        lv_obj_set_width(name, 258);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(name, sponsor_name);
        style_label(name, kVisitedText, &lv_font_montserrat_24);
        lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 65);

        lv_obj_t *table = lv_label_create(discovery_panel_);
        lv_label_set_text_fmt(table, "TABLE %02u",
                              static_cast<unsigned>(discovery_table_id_));
        style_label(table, BadgeTheme::TEXT_SOFT, &lv_font_montserrat_14);
        lv_obj_align(table, LV_ALIGN_TOP_MID, 0, 98);

        lv_obj_t *hint = lv_label_create(discovery_panel_);
        lv_label_set_text(hint, "A / B / START TO CLOSE");
        style_label(hint, BadgeTheme::TEXT_DIM, &lv_font_montserrat_14);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    void set_discovery_leds(bool on)
    {
        if (discovery_leds_on_ == on) return;
        discovery_leds_on_ = on;
        for (size_t led = 0; led < LED_COUNT; ++led) {
            hal_lights_set(led, 0, on ? 255 : 0, on ? 64 : 0);
        }
        hal_lights_show();
    }

    void tick_discovery()
    {
        ++discovery_elapsed_ticks_;
        const int flash_duration =
            kDiscoveryFlashCycleTicks * kDiscoveryFlashCount;
        const bool in_flash = discovery_elapsed_ticks_ < flash_duration;
        const bool flash_on = in_flash
            && discovery_elapsed_ticks_ % kDiscoveryFlashCycleTicks
                < kDiscoveryFlashOnTicks;
        set_discovery_leds(flash_on);

        if (--discovery_ticks_ == 0) dismiss_discovery();
    }

    void dismiss_discovery()
    {
        discovery_ticks_ = 0;
        discovery_elapsed_ticks_ = 0;
        discovery_table_id_ = 0;
        set_discovery_leds(false);
        if (discovery_scrim_) lv_obj_delete(discovery_scrim_);
        discovery_scrim_ = nullptr;
        discovery_panel_ = nullptr;
        hal_nfc_clear_last_card();
    }

    void set_notice(const char *message)
    {
        snprintf(notice_, sizeof(notice_), "%s", message ? message : "");
        notice_ticks_ = kNoticeTicks;
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *progress_label_ = nullptr;
    lv_obj_t *discovery_scrim_ = nullptr;
    lv_obj_t *discovery_panel_ = nullptr;
    std::unique_ptr<SessionData> data_;
    SponsorLayout layout_ = {};
    Mode mode_ = Mode::Map;
    Mode detail_return_mode_ = Mode::Map;
    size_t selected_table_index_ = 0;
    size_t selected_sponsor_index_ = 0;
    size_t list_offset_ = 0;
    size_t visited_count_ = 0;
    size_t map_visited_count_ = 0;
    size_t assignment_count_ = 0;
    size_t unknown_assignment_count_ = 0;
    size_t map_build_index_ = 0;
    const SponsorInfo *detail_sponsor_ = nullptr;
    SponsorTableId detail_table_id_ = 0;
    char detail_slug_[kSponsorSlugMaxLength + 1] = {};
    bool show_ids_ = false;
    bool visit_total_loaded_ = false;
    bool table_states_complete_ = false;
    bool assignments_loaded_ = false;
    bool nfc_started_ = false;
    bool start_held_ = false;
    uint32_t start_pressed_at_ms_ = 0;
    int notice_ticks_ = 0;
    int discovery_ticks_ = 0;
    int discovery_elapsed_ticks_ = 0;
    SponsorTableId discovery_table_id_ = 0;
    bool discovery_leds_on_ = false;
    char notice_[48] = {};
};

SponsorsApp s_instance;

} // namespace

App *app_sponsors_instance() { return &s_instance; }

void app_sponsors_prepare_visit(SponsorTableId table_id, bool is_new)
{
    s_pending_table = table_id;
    s_pending_new = is_new;
}
