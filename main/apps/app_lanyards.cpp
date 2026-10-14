#include "app.h"
#include "lanyard_palette.h"

extern "C" const lv_image_dsc_t icon_swag;

#include <cstddef>
#include <cstdint>

namespace {

struct RoleColours {
    const char *role;
    bool has_shirt;
    std::uint32_t shirt;
    std::uint32_t lanyard;
};

constexpr std::uint32_t kWhite = 0xf7f7f2;
constexpr std::uint32_t kGreen = 0x4caf50;
constexpr std::uint32_t kYellow = 0xf1e96f;
constexpr std::uint32_t kBlue = 0x3f8edb;

constexpr RoleColours kRoles[] = {
    {"Hacker",        true,  kWhite,                    lanyard_palette::kHacker},
    {"Volunteer",     true,  kYellow,                   lanyard_palette::kVolunteer},
    {"Mentor",        true,  kBlue,                     lanyard_palette::kMentor},
    {"Workshop Lead", true,  kBlue,                     lanyard_palette::kMentor},
    {"Sponsor",       true,  kWhite,                    lanyard_palette::kSponsor},
    {"Judge*",        true,  kBlue,                     lanyard_palette::kJudge},
    {"Organizer / VL", true, kGreen,                    lanyard_palette::kOrganizer},
    {"General",       false, 0,                         lanyard_palette::kGeneral},
    {"Visitor",       false, 0,                         lanyard_palette::kVisitor},
};

} // namespace

class LanyardsApp : public App {
public:
    const char *name() const override { return "Swag"; }
    const char *icon() const override { return "SHT"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_swag; }
    const char *slug() const override { return "swag"; }
    AppAccess access() const override { return AppAccess::Internal; }

    void on_enter(lv_obj_t *root) override
    {
        lv_obj_t *screen = lv_obj_create(root);
        lv_obj_remove_style_all(screen);
        lv_obj_set_size(screen, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(screen, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

        make_header(screen, "SHIRT", 4, 64);
        make_header(screen, "LANYARD", 70, 78);
        make_header(screen, "ROLE", 156, 154);

        make_divider(screen, 68);
        make_divider(screen, 150);

        for (std::size_t i = 0; i < sizeof(kRoles) / sizeof(kRoles[0]); ++i) {
            const int y = 29 + static_cast<int>(i) * 17;
            if (kRoles[i].has_shirt) {
                make_swatch(screen, kRoles[i].shirt, 30, y);
            } else {
                make_missing(screen, 4, y - 2, 64);
            }
            make_swatch(screen, kRoles[i].lanyard, 103, y);
            make_role(screen, kRoles[i].role, 158, y - 2);
        }

        lv_obj_t *note = lv_label_create(screen);
        lv_label_set_text(note, "* Judge also has a sticker");
        lv_obj_set_style_text_color(note, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
        lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -3);
    }

private:
    static void make_header(lv_obj_t *parent, const char *text, int x, int width)
    {
        lv_obj_t *header = lv_label_create(parent);
        lv_label_set_text(header, text);
        lv_obj_set_width(header, width);
        lv_obj_set_style_text_align(header, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(header, lv_color_white(), 0);
        lv_obj_set_style_text_font(header, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(header, x, 6);
    }

    static void make_divider(lv_obj_t *parent, int x)
    {
        lv_obj_t *divider = lv_obj_create(parent);
        lv_obj_remove_style_all(divider);
        lv_obj_set_size(divider, 1, 205);
        lv_obj_set_pos(divider, x, 6);
        lv_obj_set_style_bg_color(divider, lv_color_hex(BadgeTheme::BORDER), 0);
        lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    }

    static void make_swatch(lv_obj_t *parent, std::uint32_t color, int x, int y)
    {
        constexpr int kSwatchSize = 13;

        lv_obj_t *swatch = lv_obj_create(parent);
        lv_obj_remove_style_all(swatch);
        lv_obj_set_size(swatch, kSwatchSize, kSwatchSize);
        lv_obj_set_pos(swatch, x, y);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(swatch, 1, 0);
        lv_obj_set_style_border_color(swatch, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_clear_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
    }

    static void make_missing(lv_obj_t *parent, int x, int y, int width)
    {
        lv_obj_t *missing = lv_label_create(parent);
        lv_label_set_text(missing, "-");
        lv_obj_set_width(missing, width);
        lv_obj_set_style_text_align(missing, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(missing, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_set_style_text_font(missing, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(missing, x, y);
    }

    static void make_role(lv_obj_t *parent, const char *text, int x, int y)
    {
        lv_obj_t *role = lv_label_create(parent);
        lv_label_set_text(role, text);
        lv_obj_set_style_text_color(role, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(role, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(role, x, y);
    }
};

static LanyardsApp s_instance;
App *app_lanyards_instance() { return &s_instance; }
