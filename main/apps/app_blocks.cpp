#include "app.h"
#include "hal_config.h"
#include "hal_lights.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Blocks: a super-simple 3D block game. A tiny raycaster renders a 16x16
// voxel map (grass / dirt / stone) into a 160x120 framebuffer, upscaled
// to the screen. Up/Down walks, Left/Right turns, A digs the block ahead
// (or places the selected one), B cycles the selected block.

extern "C" const lv_image_dsc_t icon_blocks;

static constexpr uint16_t rage_rgb565(uint32_t hex)
{
    return (uint16_t)(((hex >> 16 & 0xF8) << 8) | ((hex >> 8 & 0xFC) << 3) |
                      (hex & 0xF8) >> 3);
}

static uint16_t rage_shade(uint16_t c, int num, int den)
{
    int r = ((c >> 11) & 0x1F) * num / den;
    int g = ((c >> 5) & 0x3F) * num / den;
    int b = (c & 0x1F) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

class BlocksApp : public GameApp {
public:
    const char *name() const override { return "Blocks"; }
    const char *icon() const override { return "3D"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_blocks; }
    const char *slug() const override { return "blocks"; }

    bool wants_wake_lock() const override { return true; }
    uint32_t tick_period_ms() const override { return 40; }

    void on_enter(lv_obj_t *root) override
    {
        // Heap, not a static: 160x120 RGB565 is 38 KiB, which does not fit
        // the DRAM budget as a permanent BSS resident. Allocated per visit,
        // freed on exit (same discipline as Outrun's framebuffer).
        fb_ = (uint16_t *)malloc(sizeof(uint16_t) * kW * kH);
        if (!fb_) {
            lv_obj_t *oops = lv_label_create(root);
            lv_label_set_text(oops, "Not enough memory");
            lv_obj_set_style_text_color(oops, lv_color_hex(BadgeTheme::TEXT), 0);
            lv_obj_center(oops);
            return;
        }
        memset(fb_, 0, sizeof(uint16_t) * kW * kH);
        canvas_ = lv_canvas_create(root);
        lv_canvas_set_buffer(canvas_, fb_, kW, kH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(canvas_, 320, 240);
        lv_obj_center(canvas_);
        lv_image_set_scale(canvas_, 512); // 2x nearest-ish upscale
        lv_obj_clear_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);

        hud_ = lv_label_create(root);
        lv_obj_set_style_text_color(hud_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(hud_, &lv_font_montserrat_14, 0);
        lv_obj_set_width(hud_, 300);
        lv_obj_set_style_text_align(hud_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hud_, LV_LABEL_LONG_WRAP);
        lv_obj_align(hud_, LV_ALIGN_TOP_MID, 0, 4);

        hint_ = lv_label_create(root);
        lv_label_set_text(hint_, "Up/Down: walk  Left/Right: turn  A: dig/build  B: block");
        lv_obj_set_style_text_color(hint_, lv_color_hex(BadgeTheme::TEXT_MUTED), 0);
        lv_obj_set_width(hint_, 310);
        lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(hint_, LV_LABEL_LONG_WRAP);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -6);

        reset();
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        free(fb_);
        fb_ = nullptr;
        canvas_ = hud_ = hint_ = nullptr;
        left_ = right_ = fwd_ = back_ = false;
    }

    void on_button(ButtonEvent ev) override
    {
        if (!fb_) return;   // malloc failed in on_enter; no widgets to drive
        if (ev.kind == ButtonEventKind::Pressed) {
            switch (ev.button) {
                case Button::Up: fwd_ = true; break;
                case Button::Down: back_ = true; break;
                case Button::Left: left_ = true; break;
                case Button::Right: right_ = true; break;
                case Button::A: dig_or_place(); break;
                case Button::B:
                    sel_ = sel_ % 3 + 1;
                    ring_selected();
                    draw_hud();
                    break;
                default: break;
            }
        } else {
            switch (ev.button) {
                case Button::Up: fwd_ = false; break;
                case Button::Down: back_ = false; break;
                case Button::Left: left_ = false; break;
                case Button::Right: right_ = false; break;
                default: break;
            }
        }
    }

    void on_tick() override
    {
        if (!fb_) return;   // malloc failed in on_enter; nothing to draw into
        if (left_ && !right_) ang_ -= 2.6f * DT;
        else if (right_ && !left_) ang_ += 2.6f * DT;
        float mv = 0.0f;
        if (fwd_ && !back_) mv = 3.0f * DT;
        else if (back_ && !fwd_) mv = -3.0f * DT;
        if (mv != 0.0f) {
            try_move(cosf(ang_) * mv, 0.0f);
            try_move(0.0f, sinf(ang_) * mv);
        }
        render();
    }

private:
    static constexpr int kW = 160;
    static constexpr int kH = 120;
    static constexpr int kMap = 16;
    static constexpr float DT = 0.04f;
    static constexpr float kR = 0.22f;

    static constexpr const char *kMapRows[kMap] = {
        "1111111111111111",
        "1000000000000001",
        "1022000000330001",
        "1022000000330001",
        "1000000000000001",
        "1000111000000001",
        "1000111000220001",
        "1000000000220001",
        "1000000000000001",
        "1033000000000001",
        "1033000022000001",
        "1000000002000001",
        "1000000000000001",
        "1000200000003001",
        "1000000000000001",
        "1111111111111111",
    };

    static uint16_t block_color(int t, bool xside)
    {
        uint16_t c = 0;
        if (t == 1) c = rage_rgb565(0x53d769);
        else if (t == 2) c = rage_rgb565(0x9c6b3c);
        else if (t == 3) c = rage_rgb565(0x9aa0a6);
        return xside ? rage_shade(c, 2, 3) : c;
    }

    void reset()
    {
        for (int y = 0; y < kMap; y++) {
            for (int x = 0; x < kMap; x++) {
                char c = kMapRows[y][x];
                map_[y][x] = (uint8_t)((c >= '0' && c <= '3') ? c - '0' : 0);
            }
        }
        px_ = 8.5f;
        py_ = 12.5f;
        ang_ = -1.5708f; // face north
        sel_ = 2;
        ring_selected();
        draw_hud();
        render();
    }

    int cell_at(float x, float y) const
    {
        int ix = (int)x, iy = (int)y;
        if (ix < 0 || iy < 0 || ix >= kMap || iy >= kMap) return 1;
        return map_[iy][ix];
    }

    void try_move(float dx, float dy)
    {
        float nx = px_ + dx;
        if (cell_at(nx + (dx > 0 ? kR : -kR), py_ - kR) == 0 &&
            cell_at(nx + (dx > 0 ? kR : -kR), py_) == 0 &&
            cell_at(nx + (dx > 0 ? kR : -kR), py_ + kR) == 0) {
            px_ = nx;
        }
        float ny = py_ + dy;
        if (cell_at(px_ - kR, ny + (dy > 0 ? kR : -kR)) == 0 &&
            cell_at(px_, ny + (dy > 0 ? kR : -kR)) == 0 &&
            cell_at(px_ + kR, ny + (dy > 0 ? kR : -kR)) == 0) {
            py_ = ny;
        }
    }

    void dig_or_place()
    {
        float tx = px_ + cosf(ang_) * 1.2f;
        float ty = py_ + sinf(ang_) * 1.2f;
        int ix = (int)tx, iy = (int)ty;
        if (ix <= 0 || iy <= 0 || ix >= kMap - 1 || iy >= kMap - 1) return; // border stays
        if (map_[iy][ix] != 0) map_[iy][ix] = 0;
        else map_[iy][ix] = (uint8_t)sel_;
        render();
    }

    void ring_selected()
    {
        uint32_t cols[4] = { 0, 0x53d769, 0x9c6b3c, 0x9aa0a6 };
        for (size_t i = 0; i < LED_COUNT; i++) {
            uint32_t c = cols[sel_];
            hal_lights_set(i, (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c);
        }
        hal_lights_show();
    }

    void draw_hud()
    {
        if (!hud_) return;
        const char *names[4] = { "", "GRASS", "DIRT", "STONE" };
        lv_label_set_text_fmt(hud_, "BLOCKS  [%s]", names[sel_]);
    }

    void vline(int x, int y0, int y1, uint16_t c)
    {
        if (x < 0 || x >= kW) return;
        if (y0 < 0) y0 = 0;
        if (y1 >= kH) y1 = kH - 1;
        uint16_t *p = fb_ + y0 * kW + x;
        for (int y = y0; y <= y1; y++, p += kW) *p = c;
    }

    void render()
    {
        if (!canvas_) return;
        // Sky + floor.
        for (int y = 0; y < kH / 2; y++) {
            uint16_t *p = fb_ + y * kW;
            for (int x = 0; x < kW; x++) p[x] = kSky;
        }
        for (int y = kH / 2; y < kH; y++) {
            uint16_t *p = fb_ + y * kW;
            for (int x = 0; x < kW; x++) p[x] = kFloorC;
        }
        float dirx = cosf(ang_), diry = sinf(ang_);
        float planex = -diry * 0.66f, planey = dirx * 0.66f;
        for (int x = 0; x < kW; x++) {
            float cam = 2.0f * (float)x / (float)kW - 1.0f;
            float rdx = dirx + planex * cam;
            float rdy = diry + planey * cam;
            int mapx = (int)px_, mapy = (int)py_;
            float ddx = (rdx == 0.0f) ? 1e9f : fabsf(1.0f / rdx);
            float ddy = (rdy == 0.0f) ? 1e9f : fabsf(1.0f / rdy);
            int stepx, stepy;
            float sdx, sdy;
            if (rdx < 0) {
                stepx = -1;
                sdx = (px_ - mapx) * ddx;
            } else {
                stepx = 1;
                sdx = (mapx + 1.0f - px_) * ddx;
            }
            if (rdy < 0) {
                stepy = -1;
                sdy = (py_ - mapy) * ddy;
            } else {
                stepy = 1;
                sdy = (mapy + 1.0f - py_) * ddy;
            }
            int side = 0, hit = 0, guard = 0;
            while (!hit && guard++ < 64) {
                if (sdx < sdy) {
                    sdx += ddx;
                    mapx += stepx;
                    side = 0;
                } else {
                    sdy += ddy;
                    mapy += stepy;
                    side = 1;
                }
                if (mapx < 0 || mapy < 0 || mapx >= kMap || mapy >= kMap) break;
                hit = map_[mapy][mapx];
            }
            if (!hit) continue;
            float dist = (side == 0) ? (sdx - ddx) : (sdy - ddy);
            if (dist < 0.05f) dist = 0.05f;
            int line_h = (int)((float)kH / dist);
            int y0 = kH / 2 - line_h / 2;
            int y1 = kH / 2 + line_h / 2;
            vline(x, y0, y1, block_color(hit, side == 1));
        }
        // Crosshair.
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx = kW / 2 + dx, cy = kH / 2 + dy;
                fb_[cy * kW + cx] = kCross;
            }
        }
        lv_obj_invalidate(canvas_);
    }

    static constexpr uint16_t kSky = rage_rgb565(0x0b0c0b);
    static constexpr uint16_t kFloorC = rage_rgb565(0x2b1a12);
    static constexpr uint16_t kCross = rage_rgb565(0xf4f4ef);

    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *hud_ = nullptr;
    lv_obj_t *hint_ = nullptr;
    uint16_t *fb_ = nullptr;
    uint8_t map_[kMap][kMap] = {};
    float px_ = 8.5f, py_ = 12.5f, ang_ = -1.5708f;
    int sel_ = 2;
    bool left_ = false, right_ = false, fwd_ = false, back_ = false;
};

static BlocksApp s_instance;
App *app_blocks_instance() { return &s_instance; }
