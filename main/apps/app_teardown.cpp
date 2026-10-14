#include "app.h"

extern "C" const lv_image_dsc_t icon_teardown;

#include <cstdio>

extern "C" const lv_image_dsc_t teardown_front;
extern "C" const lv_image_dsc_t teardown_back;

namespace {

// Hotspot boxes are in display pixels over the 320x200 board photos
// (assets/teardown/*.png, encoded as teardown_front/teardown_back).
// side: 0 = front (screen side), 1 = back (battery side).
struct Hotspot {
    uint8_t side;
    int16_t x0, y0, x1, y1;
    const char *title;
    const char *body;
};

constexpr Hotspot kStops[] = {
    { 0, 38, 8, 118, 178, "Screen",
      "2.0-inch ST7789 LCD, 320x240 RGB565 over 40MHz SPI. LVGL repaints "
      "only dirty 30-row DMA stripes, so idle screens sip power." },
    { 0, 153, 85, 194, 117, "Brain + radio",
      "ESP32-C3-MINI-1: 32-bit RISC-V at 160MHz, 4MB flash, Wi-Fi 4 plus "
      "BLE 5. It runs every app; this firmware lives in its factory partition." },
    { 0, 186, 24, 270, 186, "Buttons",
      "Seven face buttons share 3 ESP32 pins via a 74HC165 shift register. "
      "START is GPIO9, the BOOT strap: hold it while plugging in USB for "
      "download mode." },
    { 0, 122, 155, 168, 200, "USB + power",
      "USB-C feeds 5V plus flashing and console over native USB-Serial-JTAG: "
      "no UART chip. SRV05-4 eats static; an LM66200 ORs USB with the AA rail." },
    { 0, 3, 126, 18, 151, "Power slide",
      "The ON/OFF slide drives the MT3608 boost converter's enable pin, so "
      "OFF truly disconnects the AA rail. The JP1 solder jumper bridges it "
      "for factory fixtures." },
    { 0, 80, 180, 120, 200, "Side switch",
      "This small slide is an input, not power: firmware reads it through "
      "the shift register as Aux1. Onboarding makes you flip it to prove "
      "every input works." },
    { 0, 290, 167, 302, 178, "LEDs",
      "Six WS2812B-2020 RGB LEDs ring the board on one GPIO3 daisy chain. "
      "The ESP32 speaks 3.3V, so an SN74LVC1T45 shifter boosts data to the "
      "5V strip." },
    { 1, 42, 38, 115, 165, "Batteries",
      "Two AA alkaline cells, ~3V. USB never charges them. A P-FET blocks "
      "reverse cells and a Schottky steers the rail before the MT3608 "
      "boosts it to 5V." },
    { 1, 250, 44, 319, 159, "NFC antenna",
      "This copper loop is the 13.56MHz NFC antenna, driven differentially "
      "by the reader. Keep metal and the batteries clear of it when you tap "
      "to pair." },
    { 1, 182, 15, 260, 55, "NFC reader",
      "NXP MFRC522 reader with a 27.12MHz crystal and matching network. It "
      "shares one I2C bus with the accelerometer and only powers up when an "
      "app asks." },
};
constexpr int kStopCount = sizeof(kStops) / sizeof(kStops[0]);

constexpr int kPhotoW = 320;
constexpr int kPhotoH = 200;
constexpr int kPhotoY = 24;
// LVGL zoom units: 256 = 1x, so 512 renders the photo at 2x.
constexpr uint32_t kZoom = 512;
constexpr int kViewW = 320;
constexpr int kViewH = 120;
constexpr int kViewY = 28;

class TeardownApp final : public App {
public:
    const char *name() const override { return "Teardown"; }
    const char *icon() const override { return "HW"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_teardown; }
    const char *slug() const override { return "teardown"; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        idx_ = 0;
        detail_ = false;
        rebuild();
    }

    void on_exit() override { root_ = nullptr; }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        switch (ev.button) {
            case Button::Left:  move(-1); break;
            case Button::Right: move(1); break;
            case Button::A:
                detail_ = !detail_;
                rebuild();
                break;
            case Button::B:
                if (detail_) {
                    detail_ = false;
                    rebuild();
                }
                break;
            case Button::Up:
            case Button::Down: flip_side(); break;
            default: break;
        }
    }

private:
    void move(int dir)
    {
        idx_ = (idx_ + dir + kStopCount) % kStopCount;
        rebuild();
    }

    void flip_side()
    {
        uint8_t want = (uint8_t)(kStops[idx_].side ^ 1u);
        for (int i = 0; i < kStopCount; i++) {
            if (kStops[i].side == want) {
                idx_ = i;
                rebuild();
                return;
            }
        }
    }

    void rebuild()
    {
        if (!root_) return;
        lv_obj_clean(root_);
        lv_obj_set_style_bg_color(root_, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        const Hotspot &s = kStops[idx_];
        if (detail_) build_detail(s);
        else build_overview(s);
    }

    static lv_obj_t *plain_box(lv_obj_t *parent)
    {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        return o;
    }

    void build_overview(const Hotspot &s)
    {
        char title[96];
        snprintf(title, sizeof(title), "%d/%d %s: %s", idx_ + 1, kStopCount,
                 s.side ? "BACK" : "FRONT", s.title);
        lv_obj_t *head = lv_label_create(root_);
        lv_label_set_text(head, title);
        lv_obj_set_width(head, kPhotoW);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(head, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(head, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 4);

        lv_obj_t *wrap = plain_box(root_);
        lv_obj_set_size(wrap, kPhotoW, kPhotoH);
        lv_obj_set_pos(wrap, 0, kPhotoY);

        lv_obj_t *img = lv_image_create(wrap);
        lv_image_set_src(img, s.side ? &teardown_back : &teardown_front);
        lv_obj_set_pos(img, 0, 0);

        for (int i = 0; i < kStopCount; i++) {
            const Hotspot &h = kStops[i];
            if (h.side != s.side) continue;
            lv_obj_t *box = plain_box(wrap);
            lv_obj_set_pos(box, h.x0, h.y0);
            lv_obj_set_size(box, h.x1 - h.x0, h.y1 - h.y0);
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(box, 0, 0);
            lv_obj_set_style_border_width(box, 2, 0);
            if (i == idx_) {
                lv_obj_set_style_border_color(
                    box, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
                lv_obj_set_style_border_opa(box, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_border_color(
                    box, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
                lv_obj_set_style_border_opa(box, LV_OPA_50, 0);
            }
        }

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "L/R move - A zoom - U/D flip side");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    void build_detail(const Hotspot &s)
    {
        char title[96];
        snprintf(title, sizeof(title), "%d/%d: %s", idx_ + 1, kStopCount, s.title);
        lv_obj_t *head = lv_label_create(root_);
        lv_label_set_text(head, title);
        lv_obj_set_width(head, kPhotoW);
        lv_obj_set_style_text_align(head, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(head, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(head, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_align(head, LV_ALIGN_TOP_MID, 0, 2);

        lv_obj_t *view = plain_box(root_);
        lv_obj_set_size(view, kViewW, kViewH);
        lv_obj_set_pos(view, 0, kViewY);
        lv_obj_set_style_bg_color(view, lv_color_hex(BadgeTheme::BACKGROUND), 0);
        lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);

        int32_t cx = (s.x0 + s.x1) / 2;
        int32_t cy = (s.y0 + s.y1) / 2;
        int32_t ix = kViewW / 2 - cx;
        int32_t iy = kViewH / 2 - cy;
        // Keep the 2x (640x400) render covering the viewport: pinning the
        // position still leaves the hotspot visible, just off-center.
        if (ix > 0) ix = 0;
        if (ix < kViewW - 2 * kPhotoW) ix = kViewW - 2 * kPhotoW;
        if (iy > 0) iy = 0;
        if (iy < kViewH - 2 * kPhotoH) iy = kViewH - 2 * kPhotoH;

        lv_obj_t *img = lv_image_create(view);
        lv_image_set_src(img, s.side ? &teardown_back : &teardown_front);
        lv_image_set_pivot(img, cx, cy);
        lv_image_set_scale(img, kZoom);
        lv_image_set_antialias(img, true);
        lv_obj_set_pos(img, ix, iy);

        lv_obj_t *body = lv_label_create(root_);
        lv_label_set_text(body, s.body);
        lv_obj_set_width(body, kPhotoW - 12);
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(body, lv_color_hex(BadgeTheme::TEXT_SOFT), 0);
        lv_obj_align(body, LV_ALIGN_TOP_MID, 0, kViewY + kViewH + 4);

        lv_obj_t *hint = lv_label_create(root_);
        lv_label_set_text(hint, "B back - L/R next - U/D flip side");
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(BadgeTheme::TEXT_DIM), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    lv_obj_t *root_ = nullptr;
    int idx_ = 0;
    bool detail_ = false;
};

TeardownApp s_instance;

} // namespace

App *app_teardown_instance() { return &s_instance; }
