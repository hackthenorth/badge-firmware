#include "app.h"
#include "app_reboot.h"
#include "hal_config.h"
#include "hal_lights.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

// Outrun: a Sega-OutRun-inspired pseudo-3D sunset racer. A segment-based
// road renderer draws curves and hills into a 160x120 framebuffer, upscaled
// to the screen; pixel-art sprites (player, traffic, palms, signs) are
// billboarded in software. Left/Right steers, A is gas, B is brake. Beat
// the countdown timer; each lap adds bonus time. Best score persists.

extern "C" const lv_image_dsc_t icon_outrun;

static constexpr uint16_t out_rgb565(uint32_t hex)
{
    return (uint16_t)(((hex >> 16 & 0xF8) << 8) | ((hex >> 8 & 0xFC) << 3) |
                      (hex & 0xF8) >> 3);
}

static uint16_t out_sprite_color(char c, uint16_t body, uint16_t dark)
{
    switch (c) {
        case 'K': return out_rgb565(0x0a0a0e);
        case 'W': return out_rgb565(0xf5f5f5);
        case 'R': return out_rgb565(0xdc1e1e);
        case 'r': return out_rgb565(0x8c1010);
        case 'G': return out_rgb565(0x78c8eb);
        case 'g': return out_rgb565(0x3c78aa);
        case 'Y': return out_rgb565(0xfac828);
        case 'O': return out_rgb565(0xfa8c1e);
        case 'N': return out_rgb565(0x784b28);
        case 'P': return out_rgb565(0x1ea03c);
        case 'p': return out_rgb565(0x146e2d);
        case 'B': return body;
        case 'b': return dark;
        case 'S': return out_rgb565(0xc8c8d2);
        case 's': return out_rgb565(0x82828c);
        case 'T': return out_rgb565(0x3c3c46);
        case 'V': return out_rgb565(0xfafa78);
        default: return out_rgb565(0xff00ff);
    }
}

static constexpr const char *kPlayerMap[] = {
    "........................................",
    "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK",
    "KRWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWRK",
    "KRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRK",
    "KKR" "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG" "RKK",
    "TTKRGggggggggggggggggggggggggggggggGRKTT",
    "TTTKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKTTT",
    "TTTTKRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRKTTTT",
    "TTTTTKrrrrrrrrrrrrrrrrrrrrrrrrrrrrKTTTTT",
    "TTTTTTKRRRRRRRRRRRRRRRRRRRRRRRRRRKTTTTTT",
    "TTTTTTKRrRRRRRRRRRRRRRRRRRRRRRRrRKTTTTTT",
    "TTTTTTKRrRWRRRRRRRRRRRRRRRRRRWRrRKTTTTTT",
    "TTTTTT" "KRrRWW" "RRRRRRRRRRRRRRRR" "WWRrRK" "TTTTTT",
    "TTTTTTTT" "KrRW" "RRRRRRRRRRRRRRRR" "WRrK" "TTTTTTTT",
    "TTTTTTTTT" "KrRW" "RRRRRRRRRRRRRR" "WRrK" "TTTTTTTTT",
    "TTTTTTTTTT" "KrRRR" "WWWWWWWWWW" "RRRrK" "TTTTTTTTTT",
    ".TTTTTTTTKRrRRRRRRRRRRRRRRRRrrKTTTTTTTT.",
    ".TTTTTTTTTKVVKRRRRRRRRRRRRKVVKTTTTTTTTT.",
    "..TTTTTTTTTTKKKKKKKKKKKKKKKKTTTTTTTTTT..",
    "..TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT..",
    "...TTTTTTTTTTKKKKKKKKKKKKKKTTTTTTTTTT...",
    "........................................",
};

static constexpr const char *kTrafficMap[] = {
    "..............................",
    "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",
    "BWWWWWWWWWWWWWWWWWWWWWWWWWWWWB",
    "BWBBBBBBBBBBBBBBBBBBBBBBBBBBWB",
    "TBGggggggggggggggggggggggggGBT",
    "TTBBBBBBBBBBBBBBBBBBBBBBBBBBTT",
    "TTTBBBbBBBBBBBBBBBBBBBBbBBTTTT",
    "TTTTBBbBBWWBBBBBBBBWWBBbBBTTTT",
    "TTTTTTBbBWWBBBBBBBBWWBbBTTTTTT",
    "TTTTTTTBbBBWWWWWWWWBBbBTTTTTTT",
    ".TTTTTTTBbBBBBBBBBBBbBTTTTTTT.",
    ".TTTTTTTTBVBBBBBBBBBVBBTTTTTT.",
    "..TTTTTTTKBBBBBBBBBBBKTTTTTT..",
    "...TTTTTTKKKKKKKKKKKKTTTTTT...",
    "..............................",
};

static constexpr const char *kPalmMap[] = {
    "......PPPPPP..........",
    "....PPPPPPPPPP........",
    "..PPPPPPPPPPPPPP......",
    ".PPPPpPPPPPPpPPP......",
    ".PPpPpPPPPPPpPpPP.....",
    "..PPpPPPPPPPPpPP......",
    "...PPPPPPPPPPPP.......",
    ".....PPPPPPPP.........",
    "...pPPPPPPPPp.........",
    "..pPpPPPPPPpPp........",
    "..PpPPPPPPPPpP........",
    "..PPPPPPPPPPPP........",
    "....pPPPPPPp..........",
    ".....PPPPPP...........",
    "......PPPP............",
    ".......NNN............",
    ".......NNN............",
    ".......NNN............",
    ".......NNN............",
    ".......NNN............",
    ".......NNN............",
    "......NNNNN...........",
};

static constexpr const char *kSignMap[] = {
    "WWWWWWWWWWWWWWWW",
    "WOOOOOOOOOOOOOOW",
    "WOKKKKKKKKKKKKOW",
    "WOKWWKKKKKKWWKOW",
    "WOKWWKKKKKKWWKOW",
    "WOKKKKKKKKKKKKOW",
    "WOKKWWKKKKWWKKOW",
    "WOKKWWKKKKWWKKOW",
    "WOKKKKKKKKKKKKOW",
    "WOOOOOOOOOOOOOOW",
    "WWWWWWWWWWWWWWWW",
    "......KKKK......",
    "......KKKK......",
    "......KKKK......",
    "......KKKK......",
};

class OutrunApp : public GameApp {
public:
    const char *name() const override { return "Outrun"; }
    const char *icon() const override { return "Race"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_outrun; }
    const char *slug() const override { return "outrun"; }

    bool wants_wake_lock() const override { return true; }
    uint32_t tick_period_ms() const override { return 50; }

    void on_enter(lv_obj_t *root) override
    {
        // Both the framebuffer and track scratch belong to this visit. Keep
        // the 16.2 KB track arrays out of every other app's RAM budget.
        fb_ = (uint16_t *)malloc(sizeof(uint16_t) * kW * kH);
        if (fb_) track_.reset(new (std::nothrow) TrackData{});
        if (!fb_ || !track_) {
            free(fb_);
            fb_ = nullptr;
            track_.reset();
            // Preserve the game's existing fresh-heap recovery path, after
            // releasing any part of this visit that did allocate.
            app_reboot_into_app(this);
            lv_obj_t *oops = lv_label_create(root);
            lv_label_set_text(oops, "Not enough memory\nHOME for Apps");
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
        lv_obj_align(hud_, LV_ALIGN_TOP_MID, 0, 2);

        sub_ = lv_label_create(root);
        lv_obj_set_style_text_color(sub_, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(sub_, &lv_font_montserrat_14, 0);
        lv_obj_set_width(sub_, 300);
        lv_obj_set_style_text_align(sub_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(sub_, LV_ALIGN_BOTTOM_MID, 0, -2);

        banner_ = lv_label_create(root);
        lv_obj_set_style_text_color(banner_, lv_color_hex(0xffd868), 0);
        lv_obj_set_style_text_font(banner_, &lv_font_montserrat_24, 0);
        lv_obj_set_width(banner_, 300);
        lv_obj_set_style_text_align(banner_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(banner_, LV_LABEL_LONG_WRAP);
        lv_obj_align(banner_, LV_ALIGN_CENTER, 0, -30);

        build_track();
        best_ = hal_config_get_int("outrun", "best", 0);
        reset_race();
        state_ = State::Title;
    }

    void on_exit() override
    {
        hal_lights_clear();
        hal_lights_show();
        free(fb_);
        fb_ = nullptr;
        track_.reset();
        canvas_ = hud_ = sub_ = banner_ = nullptr;
        left_ = right_ = gas_ = brake_ = false;
    }

    void checkpoint_progress() override
    {
        if (!fb_) return;
        score_ = (int)(dist_ / 100.0f);
        if (score_ <= best_) return;
        best_ = score_;
        hal_config_set_int("outrun", "best", best_);
    }

    void on_button(ButtonEvent ev) override
    {
        if (!fb_) return;   // malloc failed in on_enter; no widgets to drive
        bool pressed = (ev.kind == ButtonEventKind::Pressed);
        switch (ev.button) {
            case Button::Left: left_ = pressed; break;
            case Button::Right: right_ = pressed; break;
            case Button::A: gas_ = pressed; break;
            case Button::B: brake_ = pressed; break;
            default: break;
        }
        if (!pressed) return;
        if (ev.button == Button::A) {
            if (state_ == State::Title || state_ == State::Over) {
                reset_race();
                state_ = State::Countdown;
                count_ = 3.999f;
            }
        }
    }

    void on_tick() override
    {
        if (!fb_) return;   // malloc failed in on_enter; nothing to draw into
        frame_++;
        update();
        render();
        update_hud();
        update_lights();
    }

private:
    enum class State { Title, Countdown, Race, Over };

    static constexpr int kW = 160;
    static constexpr int kH = 120;
    static constexpr int kHorizon = 54;
    static constexpr float DT = 0.05f;
    static constexpr float kSegLen = 200.0f;
    static constexpr float kRoadW = 2200.0f;
    static constexpr float kCamH = 1100.0f;
    static constexpr float kCamDepth = 0.8391f; // 1/tan(50deg)
    static constexpr float kPlayerZ = kCamH * kCamDepth;
    static constexpr int kDraw = 90;
    static constexpr int kRumble = 3;
    static constexpr float kMaxSpeed = 12000.0f;
    static constexpr int kSegCount = 900;
    static constexpr float kTrackLen = kSegCount * 200.0f;

    struct TrackData {
        float curve[kSegCount];
        float elev[kSegCount];
        float segCX[kSegCount];
        float segS[kSegCount];
        int16_t segY[kSegCount];
    };

    struct Car {
        float z = 0.0f;
        float offset = 0.0f;
        float speed = 5000.0f;
        uint8_t color = 0;
    };

    void build_track()
    {
        int n = 0;
        auto add_road = [&](int enter, int hold, int leave, float curve, float dy) {
            float y0 = (n > 0) ? track_->elev[n - 1] : 0.0f;
            float y1 = y0 + dy;
            for (int i = 0; i < enter && n < kSegCount; i++, n++) {
                float t = (float)i / (float)enter;
                track_->curve[n] = curve * t;
                track_->elev[n] = y0 + (y1 - y0) * t * t;
            }
            for (int i = 0; i < hold && n < kSegCount; i++, n++) {
                track_->curve[n] = curve;
                track_->elev[n] = y1;
            }
            for (int i = 0; i < leave && n < kSegCount; i++, n++) {
                float t = (float)i / (float)leave;
                track_->curve[n] = curve * (1.0f - t);
                track_->elev[n] = y1 + (y0 - y1) * (1.0f - (1.0f - t) * (1.0f - t));
            }
        };
        add_road(60, 60, 60, 0.0f, 0.0f);
        add_road(50, 50, 50, 2.0f, 0.0f);
        add_road(40, 40, 40, -3.0f, 800.0f);
        add_road(50, 50, 50, 0.0f, -800.0f);
        add_road(40, 40, 40, 4.0f, 400.0f);
        add_road(60, 60, 60, 0.0f, -400.0f);
        while (n < kSegCount) { // pad any remainder straight
            track_->curve[n] = 0.0f;
            track_->elev[n] = track_->elev[n - 1];
            n++;
        }
    }

    void reset_race()
    {
        position_ = 0.0f;
        dist_ = 0.0f;
        player_x_ = 0.0f;
        speed_ = 0.0f;
        timer_ = 60.0f;
        stage_ = 1;
        score_ = 0;
        steer_ = 0.0f;
        count_ = 0.0f;
        banner_t_ = 0.0f;
        go_t_ = 0.0f;
        shake_t_ = 0.0f;
        crash_cd_ = 0.0f;
        static const float kSpeeds[6] = { 4500, 5500, 6500, 5000, 6000, 7000 };
        static const float kOffs[6] = { -0.6f, 0.0f, 0.6f, -0.3f, 0.3f, 0.0f };
        for (int i = 0; i < 6; i++) {
            cars_[i].z = (float)(i + 1) * kTrackLen / 7.0f;
            cars_[i].offset = kOffs[i];
            cars_[i].speed = kSpeeds[i];
            cars_[i].color = (uint8_t)(i % 3);
        }
    }

    void update()
    {
        if (state_ == State::Title) {
            steer_ = 0.0f;
            position_ += 4500.0f * DT;
            if (position_ >= kTrackLen) position_ -= kTrackLen;
            player_x_ = sinf((float)frame_ * 0.03f) * 0.35f;
            speed_ = 4500.0f;
            for (int i = 0; i < 6; i++) {
                cars_[i].z += cars_[i].speed * DT;
                if (cars_[i].z >= kTrackLen) cars_[i].z -= kTrackLen;
            }
            return;
        }
        if (state_ == State::Countdown) {
            steer_ = 0.0f;
            count_ -= DT;
            speed_ = 0.0f;
            if (count_ <= 0.999f && state_ == State::Countdown) {
                state_ = State::Race;
                go_t_ = 1.2f; // GO!
            }
            return;
        }
        if (state_ == State::Over) {
            steer_ = 0.0f;
            speed_ = 0.0f;
            return;
        }
        // Race.
        float pct = speed_ / kMaxSpeed;
        if (gas_ && !brake_) speed_ += 7000.0f * DT;
        else if (brake_) speed_ -= 12000.0f * DT;
        else speed_ -= 2500.0f * DT;
        bool offroad = (player_x_ < -1.0f || player_x_ > 1.0f);
        float cap = offroad ? 3000.0f : kMaxSpeed;
        if (speed_ > cap) speed_ = cap;
        if (speed_ < 0.0f) speed_ = 0.0f;
        if (offroad && speed_ > 500.0f && (frame_ & 3) == 0) shake_t_ = 0.1f;

        float steer = 0.0f;
        if (left_ && !right_) steer = -1.0f;
        else if (right_ && !left_) steer = 1.0f;
        steer_ = steer;
        player_x_ += steer * DT * 2.4f * (0.35f + 0.65f * pct);
        int base_idx = (int)(position_ / kSegLen) % kSegCount;
        player_x_ -= track_->curve[base_idx] * pct * DT * 1.1f;
        if (player_x_ < -2.2f) player_x_ = -2.2f;
        if (player_x_ > 2.2f) player_x_ = 2.2f;

        position_ += speed_ * DT;
        dist_ += speed_ * DT;
        if (position_ >= kTrackLen) {
            position_ -= kTrackLen;
            stage_++;
            timer_ += 25.0f;
            if (timer_ > 99.0f) timer_ = 99.0f;
            banner_t_ = 2.0f;
        }
        timer_ -= DT;
        if (timer_ <= 0.0f) {
            timer_ = 0.0f;
            state_ = State::Over;
            score_ = (int)(dist_ / 100.0f);
            checkpoint_progress();
            return;
        }
        score_ = (int)(dist_ / 100.0f);

        if (crash_cd_ > 0.0f) crash_cd_ -= DT;
        if (shake_t_ > 0.0f) shake_t_ -= DT;
        if (go_t_ > 0.0f) go_t_ -= DT;
        for (int i = 0; i < 6; i++) {
            cars_[i].z += cars_[i].speed * DT;
            if (cars_[i].z >= kTrackLen) cars_[i].z -= kTrackLen;
            // Keep the pack ahead of the player so traffic always enters
            // from the horizon instead of popping in beside the car.
            float ahead = cars_[i].z - position_;
            if (ahead < 0.0f) ahead += kTrackLen;
            if (ahead > kTrackLen * 0.6f) {
                cars_[i].z = position_ + kTrackLen * 0.35f + (float)(i * 7000);
                if (cars_[i].z >= kTrackLen) cars_[i].z -= kTrackLen;
                cars_[i].offset = (float)(((frame_ >> 3) + i * 37) % 13 - 6) / 10.0f;
            }
            if (crash_cd_ <= 0.0f && speed_ > 2000.0f) {
                float rel = cars_[i].z - position_;
                if (rel < -kTrackLen / 2) rel += kTrackLen;
                if (rel > kTrackLen / 2) rel -= kTrackLen;
                if (rel > -200.0f && rel < 200.0f &&
                    fabsf(cars_[i].offset - player_x_) < 0.35f) {
                    speed_ = 1200.0f;
                    shake_t_ = 0.6f;
                    crash_cd_ = 1.0f;
                }
            }
        }
        if (banner_t_ > 0.0f) banner_t_ -= DT;
    }

    void hline(int x0, int x1, int y, uint16_t c)
    {
        if (y < 0 || y >= kH) return;
        if (x0 < 0) x0 = 0;
        if (x1 >= kW) x1 = kW - 1;
        if (x1 < x0) return;
        uint16_t *p = fb_ + y * kW + x0;
        for (int x = x0; x <= x1; x++) *p++ = c;
    }

    void draw_sprite(const char *const *map, int sw, int sh, int dx, int dy, float scale,
                     uint16_t body, uint16_t dark, int clip)
    {
        int dw = (int)(sw * scale), dh = (int)(sh * scale);
        if (dw < 2 || dh < 2 || dh > 130) return;
        for (int y = 0; y < dh; y++) {
            int yy = dy + y;
            if (yy < 0) continue;
            if (yy >= kH || yy >= clip) break;
            int sy = (int)((float)y / scale);
            if (sy >= sh) break;
            const char *row = map[sy];
            for (int x = 0; x < dw; x++) {
                int xx = dx + x;
                if (xx < 0) continue;
                if (xx >= kW) break;
                char c = row[(int)((float)x / scale)];
                if (c == '.') continue;
                fb_[yy * kW + xx] = out_sprite_color(c, body, dark);
            }
        }
    }

    void render()
    {
        if (!canvas_) return;
        for (int i = 0; i < kSegCount; i++) track_->segY[i] = -1;
        // Sky gradient.
        for (int y = 0; y < kHorizon; y++) {
            float t = (float)y / (float)(kHorizon - 1);
            int r, g, b;
            if (t < 0.5f) {
                float u = t * 2.0f;
                r = (int)(25 + (150 - 25) * u);
                g = (int)(10 + (40 - 10) * u);
                b = (int)(60 + (90 - 60) * u);
            } else {
                float u = (t - 0.5f) * 2.0f;
                r = (int)(150 + (255 - 150) * u);
                g = (int)(40 + (140 - 40) * u);
                b = (int)(90 + (60 - 90) * u);
            }
            uint16_t c = out_rgb565((uint32_t)((r << 16) | (g << 8) | b));
            uint16_t *p = fb_ + y * kW;
            for (int x = 0; x < kW; x++) p[x] = c;
        }
        // Striped sun.
        {
            int sx = 112, sy = 30, sr = 13;
            uint16_t sun = out_rgb565(0xffdc78);
            uint16_t core = out_rgb565(0xfff5d2);
            for (int dy = -sr; dy < sr; dy++) {
                int hw = (int)sqrtf((float)(sr * sr - dy * dy));
                hline(sx - hw, sx + hw, sy + dy, (dy * dy < 18) ? core : sun);
            }
            uint16_t slit = out_rgb565(0xff8c3c);
            hline(sx - 12, sx + 12, sy + 4, slit);
            hline(sx - 13, sx + 13, sy + 7, slit);
            hline(sx - 11, sx + 11, sy + 10, slit);
        }
        // Mountain silhouette + drifting clouds.
        {
            uint16_t mtn = out_rgb565(0x2a1040);
            for (int x = 0; x < kW; x++) {
                int mh = kHorizon - 4 -
                         (int)(3.0f * sinf((float)x * 0.11f) +
                               2.0f * sinf((float)x * 0.043f + 1.7f));
                for (int y = mh; y < kHorizon; y++) fb_[y * kW + x] = mtn;
            }
            uint16_t cl = out_rgb565(0xf8c8d8);
            int c1x = (int)((frame_ / 4) % 240) - 40;
            int c2x = 200 - (int)((frame_ / 5) % 240);
            hline(c1x - 11, c1x + 11, 16, cl);
            hline(c1x - 15, c1x + 15, 17, cl);
            hline(c1x - 15, c1x + 15, 18, cl);
            hline(c1x - 11, c1x + 11, 19, cl);
            hline(c2x - 9, c2x + 9, 30, cl);
            hline(c2x - 12, c2x + 12, 31, cl);
            hline(c2x - 12, c2x + 12, 32, cl);
            hline(c2x - 9, c2x + 9, 33, cl);
        }
        // Grass base.
        for (int y = kHorizon; y < kH; y++) {
            uint16_t *p = fb_ + y * kW;
            for (int x = 0; x < kW; x++) p[x] = kGrassB;
        }

        float pos = position_;
        if (pos < 0.0f) pos = 0.0f;
        int base_idx = (int)(pos / kSegLen) % kSegCount;
        float base_y = track_->elev[base_idx];
        float cam_z = pos - kPlayerZ;
        float cam_x = player_x_ * kRoadW;
        if (shake_t_ > 0.0f) {
            float j = (float)((frame_ * 1103515245u + 12345u) % 100) / 100.0f - 0.5f;
            cam_x += j * 160.0f;
        }
        float x = 0.0f;
        float dx = -(track_->curve[base_idx] * fmodf(pos, kSegLen) / kSegLen);
        int maxy = kH;
        for (int n = 0; n < kDraw; n++) {
            int idx = base_idx + n;
            if (idx >= kSegCount) idx -= kSegCount;
            float curve = track_->curve[idx];
            float wz1 = (float)idx * kSegLen - cam_z + (idx < base_idx ? kTrackLen : 0.0f);
            float wz2 = wz1 + kSegLen;
            if (wz1 < 1.0f) {
                x += dx;
                dx += curve;
                continue;
            }
            float s1 = kCamDepth / wz1, s2 = kCamDepth / wz2;
            float px1 = (x + dx) * kRoadW - cam_x;
            float py1 = (track_->elev[idx] - base_y) - kCamH;
            float X1 = kW / 2 + s1 * px1 * (kW / 2);
            float Y1 = kH / 2 - s1 * py1 * (kH / 2);
            float S1 = s1 * (kW / 2);
            x += dx;
            dx += curve;
            float px2 = (x + dx) * kRoadW - cam_x;
            int idx2 = idx + 1 >= kSegCount ? idx + 1 - kSegCount : idx + 1;
            float py2 = (track_->elev[idx2] - base_y) - kCamH;
            float X2 = kW / 2 + s2 * px2 * (kW / 2);
            float Y2 = kH / 2 - s2 * py2 * (kH / 2);
            float S2 = s2 * (kW / 2);
            if (Y2 >= Y1) continue;  // back face over a crest
            if (Y2 >= maxy) continue; // hidden by a hill
            int y_top = (int)Y2 < 0 ? 0 : (int)Y2;
            int y_bot = (int)Y1;
            if (y_bot > maxy - 1) y_bot = maxy - 1;
            if (y_bot > kH - 1) y_bot = kH - 1;
            if (y_bot < y_top) {
                maxy = (int)Y1;
                continue;
            }
            bool alt = (kRumble <= (idx % (kRumble * 2)));
            uint16_t road = alt ? kRoadA : kRoadB;
            uint16_t rum = alt ? kRumA : kRumB;
            uint16_t grass = alt ? kGrassA : kGrassB;
            for (int y = y_top; y <= y_bot; y++) {
                float t = (float)(y - Y2) / (Y1 - Y2);
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float cx = X2 + (X1 - X2) * t;
                float hw = (S2 + (S1 - S2) * t) * kRoadW;
                float rw = hw + hw / 6.0f;
                hline((int)(cx - rw), (int)(cx + rw), y, rum);
                hline((int)(cx - hw), (int)(cx + hw), y, road);
                if (alt) {
                    int lw = hw > 40.0f ? 2 : 1;
                    int off = (int)(hw / 3.0f);
                    hline((int)cx - off - lw, (int)cx - off + lw, y, kLane);
                    hline((int)cx + off - lw, (int)cx + off + lw, y, kLane);
                }
                int gx0 = (int)(cx - rw), gx1 = (int)(cx + rw);
                if (gx0 > 0) hline(0, gx0 - 1, y, grass);
                if (gx1 < kW - 1) hline(gx1 + 1, kW - 1, y, grass);
            }
            track_->segCX[idx] = X1;
            track_->segS[idx] = S1;
            track_->segY[idx] = (int16_t)y_bot;
            maxy = (int)Y1;
        }
        // Roadside sprites, far to near.
        for (int n = kDraw - 1; n >= 0; n--) {
            int idx = base_idx + n;
            if (idx >= kSegCount) idx -= kSegCount;
            if (track_->segY[idx] < 0) continue;
            int k = idx / 12;
            if (idx % 12 != 6) continue;
            int side = (k % 2) ? 1 : -1;
            bool sign = (k % 5 == 4);
            float jitter = (float)((idx * 73) % 100) / 100.0f;
            float world_off = (float)side * kRoadW * (1.55f + jitter * 0.5f);
            float S = track_->segS[idx];
            float dh = S * (sign ? 1100.0f : 2200.0f);
            if (dh < 6.0f) continue;
            float scale = dh / (sign ? 15.0f : 22.0f);
            int dw = (int)((sign ? 16.0f : 22.0f) * scale);
            int sx = (int)(track_->segCX[idx] + S * world_off) - dw / 2;
            int sy = track_->segY[idx] - (int)dh + 3;
            if (sign) draw_sprite(kSignMap, 16, 15, sx, sy, scale, 0, 0, track_->segY[idx] + 1);
            else draw_sprite(kPalmMap, 22, 22, sx, sy, scale, 0, 0, track_->segY[idx] + 1);
        }
        // Traffic, far to near.
        {
            int order[6] = { 0, 1, 2, 3, 4, 5 };
            float rels[6];
            for (int i = 0; i < 6; i++) {
                float rel = cars_[i].z - pos;
                if (rel < 0.0f) rel += kTrackLen;
                rels[i] = rel;
            }
            for (int a = 0; a < 6; a++)
                for (int b = a + 1; b < 6; b++)
                    if (rels[order[b]] > rels[order[a]]) {
                        int t = order[a];
                        order[a] = order[b];
                        order[b] = t;
                    }
            for (int o = 0; o < 6; o++) {
                int i = order[o];
                float rel = rels[i];
                if (rel < 300.0f) continue; // behind / inside the camera
                int idx = (int)(cars_[i].z / kSegLen) % kSegCount;
                if (track_->segY[idx] < 0) continue;
                // Ground the car on the drawn road: segment screen center
                // already includes curve offsets and camera shake.
                float S = track_->segS[idx];
                float w = S * 950.0f;
                if (w < 5.0f) continue;
                float sx = track_->segCX[idx] + S * cars_[i].offset * kRoadW;
                float scale = w / 30.0f;
                int dw = (int)(30.0f * scale), dh = (int)(15.0f * scale);
                static const uint16_t kBody[3] = {
                    out_rgb565(0x2850dc), out_rgb565(0xfac828), out_rgb565(0xf0f0f0)
                };
                static const uint16_t kDark[3] = {
                    out_rgb565(0x18328c), out_rgb565(0xc8901e), out_rgb565(0x82828c)
                };
                uint8_t c = cars_[i].color % 3;
                draw_sprite(kTrafficMap, 30, 15, (int)sx - dw / 2,
                            track_->segY[idx] - dh + 2, scale, kBody[c], kDark[c],
                            track_->segY[idx] + 1);
            }
        }
        // Player car.
        {
            int dxp = kW / 2 - 20 + (int)(steer_ * 3.0f);
            int dyp = kH - 22 - 18 - (((frame_ >> 2) & 1) && speed_ > 1000.0f ? 1 : 0);
            draw_sprite(kPlayerMap, 40, 22, dxp, dyp, 1.0f,
                        out_rgb565(0xdc1e1e), out_rgb565(0x8c1010), kH);
        }
        lv_obj_invalidate(canvas_);
    }

    void update_hud()
    {
        if (!hud_ || !sub_ || !banner_) return;
        if (state_ == State::Title) {
            lv_label_set_text(hud_, "OUTRUN");
            lv_label_set_text_fmt(sub_, "A: start   Best %d", best_);
            lv_label_set_text(banner_, "");
        } else if (state_ == State::Countdown) {
            lv_label_set_text_fmt(hud_, "TIME %d  STG %d", (int)timer_, stage_);
            lv_label_set_text(sub_, "A: gas  B: brake");
            int digit = (int)count_;
            if (digit < 1) digit = 1;
            if (digit > 3) digit = 3;
            char buf[2] = { (char)('0' + digit), 0 };
            lv_label_set_text(banner_, buf);
        } else if (state_ == State::Race) {
            lv_label_set_text_fmt(hud_, "TIME %d  STG %d", (int)ceilf(timer_), stage_);
            int kmh = (int)(speed_ / kMaxSpeed * 280.0f);
            lv_label_set_text_fmt(sub_, "%d km/h  SCORE %d", kmh, score_);
            if (go_t_ > 0.0f) lv_label_set_text(banner_, "GO!");
            else if (banner_t_ > 0.0f) lv_label_set_text_fmt(banner_, "STAGE %d", stage_);
            else lv_label_set_text(banner_, "");
        } else {
            lv_label_set_text(hud_, "TIME UP");
            lv_label_set_text_fmt(sub_, "Score %d Best %d  A: retry", score_, best_);
            lv_label_set_text(banner_, "");
        }
    }

    void update_lights()
    {
        uint8_t r = 0, g = 0, b = 0;
        if (state_ == State::Race) {
            if (shake_t_ > 0.35f && (frame_ & 2)) {
                r = 255; // crash flash
            } else {
                float pct = speed_ / kMaxSpeed;
                r = (uint8_t)(pct * 255.0f);
                g = (uint8_t)((1.0f - pct) * 255.0f);
            }
        } else if (state_ == State::Countdown) {
            if (frame_ & 4) g = 255;
        } else if (state_ == State::Title) {
            r = 200;
            g = (uint8_t)(90 + 60 * sinf((float)frame_ * 0.1f));
            b = 20;
        } else {
            b = 60;
        }
        for (size_t i = 0; i < LED_COUNT; i++) hal_lights_set(i, r, g, b);
        hal_lights_show();
    }

    static constexpr uint16_t kRoadA = out_rgb565(0x6e6e76);
    static constexpr uint16_t kRoadB = out_rgb565(0x68686e);
    static constexpr uint16_t kRumA = out_rgb565(0xf0f0f0);
    static constexpr uint16_t kRumB = out_rgb565(0xc82828);
    static constexpr uint16_t kGrassA = out_rgb565(0x288c3c);
    static constexpr uint16_t kGrassB = out_rgb565(0x248036);
    static constexpr uint16_t kLane = out_rgb565(0xfafafa);

    lv_obj_t *canvas_ = nullptr;
    lv_obj_t *hud_ = nullptr;
    lv_obj_t *sub_ = nullptr;
    lv_obj_t *banner_ = nullptr;
    uint16_t *fb_ = nullptr;
    std::unique_ptr<TrackData> track_;
    Car cars_[6];
    State state_ = State::Title;
    float position_ = 0.0f, dist_ = 0.0f, player_x_ = 0.0f, speed_ = 0.0f;
    float timer_ = 60.0f, count_ = 0.0f, banner_t_ = 0.0f, go_t_ = 0.0f;
    float shake_t_ = 0.0f, crash_cd_ = 0.0f, steer_ = 0.0f;
    int stage_ = 1, score_ = 0, best_ = 0;
    uint32_t frame_ = 0;
    bool left_ = false, right_ = false, gas_ = false, brake_ = false;
};

static OutrunApp s_instance;
App *app_outrun_instance() { return &s_instance; }
