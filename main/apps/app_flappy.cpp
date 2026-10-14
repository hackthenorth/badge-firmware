#include "app.h"
#include "hal_lights.h"
#include "app_registry.h"
#include "game_pause.h"
#include "hal_config.h"

#include "esp_random.h"
#include "lvgl.h"
#include <cstdint>

static constexpr int kScreenW = 320;
static constexpr int kScreenH = 240;
static constexpr int kTopGuard = 24;     // y below title/score row
static constexpr int kGroundH = 4;
static constexpr int kGroundY = kScreenH - kGroundH;
static constexpr int kBirdX = 80;
static constexpr int kBirdSize = 12;
static constexpr int kPipeW = 38;
static constexpr int kPipeGap = 74;
static constexpr int kPipeCount = 3;
static constexpr int kPipeSpacing = 120;
static constexpr float kGravity = 0.35f;
static constexpr float kFlap = -4.2f;
static constexpr float kScrollSpeed = 2.0f;
static constexpr float kMaxFallVy = 6.0f;

extern "C" const lv_image_dsc_t icon_flappy;

class FlappyApp : public GameApp {
public:
    const char *name() const override { return "Flappy"; }
    const char *icon() const override { return "FLP"; }
    const char *slug() const override { return "flappy"; }
    const lv_image_dsc_t *icon_image() const override { return &icon_flappy; }
    bool icon_needs_outline() const override { return true; }
    bool wants_wake_lock() const override { return true; }

    void on_enter(lv_obj_t *root) override
    {
        hi_ = hal_config_get_int("flappy", "high_score", 0);
        saved_hi_ = hi_;

        // Keep the moving playfield in one lightweight draw object. Updating
        // seven independent LVGL objects every 20 ms caused repeated layout
        // work and fragmented display invalidations.
        playfield_ = lv_obj_create(root);
        lv_obj_remove_style_all(playfield_);
        lv_obj_set_size(playfield_, kScreenW, kScreenH);
        lv_obj_set_pos(playfield_, 0, 0);
        lv_obj_clear_flag(playfield_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(playfield_, playfield_draw_event,
                            LV_EVENT_DRAW_MAIN, this);

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Flappy");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 4);
        game_pause_create_hint(root, LV_ALIGN_TOP_MID, 0, 5);

        score_ = lv_label_create(root);
        lv_obj_set_style_text_font(score_, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(score_, lv_color_hex(BadgeTheme::ACCENT_DETAIL), 0);
        lv_obj_align(score_, LV_ALIGN_TOP_RIGHT, -10, 4);

        overlay_ = lv_label_create(root);
        lv_obj_set_style_text_font(overlay_, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(overlay_, lv_color_hex(0xffd166), 0);
        lv_obj_align(overlay_, LV_ALIGN_CENTER, 0, 0);

        pause_.create(root);

        reset_game();
        render_full();
    }

    void on_exit() override
    {
        persist_high_score();
        pause_.on_exit();
        playfield_ = score_ = overlay_ = nullptr;
    }

    void on_button(ButtonEvent ev) override
    {
        if (pause_.handle(ev, !dead_)) {
            if (pause_.paused()) checkpoint_progress();
            return;
        }
        if (pause_.paused()) return;
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (dead_) {
            if (ev.button == Button::A) {
                persist_high_score();
                reset_game();
                render_full();
            }
            return;
        }
        if (ev.button == Button::A || ev.button == Button::Up) {
            waiting_ = false;
            vy_ = kFlap;
            refresh_text();
        }
    }

    void checkpoint_progress() override
    {
        if (score_val_ > hi_) hi_ = score_val_;
        persist_high_score();
        refresh_text();
    }

    void on_tick() override
    {
        if (pause_.paused()) return;
        if (dead_ || waiting_) return;

        const float old_bird_y = bird_y_;
        float old_pipe_x[kPipeCount];
        int old_pipe_gap_y[kPipeCount];
        for (int i = 0; i < kPipeCount; ++i) {
            old_pipe_x[i] = pipe_x_[i];
            old_pipe_gap_y[i] = pipe_gap_y_[i];
        }

        vy_ += kGravity;
        if (vy_ > kMaxFallVy) vy_ = kMaxFallVy;
        bird_y_ += vy_;

        for (int i = 0; i < kPipeCount; i++) {
            pipe_x_[i] -= kScrollSpeed;
            if (!pipe_scored_[i] && pipe_x_[i] + kPipeW < kBirdX) {
                pipe_scored_[i] = 1;
                score_val_++;
                if (score_val_ % 10 == 0) hal_lights_flash_success();
            }
            if (pipe_x_[i] + kPipeW < 0) {
                float max_x = 0;
                for (int j = 0; j < kPipeCount; j++)
                    if (pipe_x_[j] > max_x) max_x = pipe_x_[j];
                pipe_x_[i] = max_x + kPipeSpacing;
                pipe_gap_y_[i] = random_gap_y();
                pipe_scored_[i] = 0;
            }
        }

        bool collided = false;
        if (bird_y_ < kTopGuard) {
            bird_y_ = kTopGuard;
            collided = true;
        }
        if (bird_y_ + kBirdSize > kGroundY) {
            bird_y_ = kGroundY - kBirdSize;
            collided = true;
        }

        if (!collided) {
            for (int i = 0; i < kPipeCount; i++) {
                float px = pipe_x_[i];
                int py = pipe_gap_y_[i];
                if (kBirdX + kBirdSize > px && (float)kBirdX < px + kPipeW) {
                    if (bird_y_ < py || bird_y_ + kBirdSize > py + kPipeGap) {
                        collided = true;
                        break;
                    }
                }
            }
        }

        invalidate_motion(old_bird_y, old_pipe_x, old_pipe_gap_y);
        if (collided) {
            die();
            return;
        }
        refresh_text();
    }

private:
    int random_gap_y() const
    {
        int min_y = kTopGuard + 8;
        int max_y = kGroundY - kPipeGap - 8;
        int range = max_y - min_y;
        if (range < 1) range = 1;
        return min_y + (int)(esp_random() % (uint32_t)range);
    }

    void reset_game()
    {
        bird_y_ = kScreenH / 2 - kBirdSize / 2;
        vy_ = 0;
        score_val_ = 0;
        dead_ = false;
        waiting_ = true;
        rendered_score_ = -1;
        rendered_hi_ = -1;
        rendered_overlay_ = OverlayState::Invalid;
        pause_.reset();
        for (int i = 0; i < kPipeCount; i++) {
            pipe_x_[i] = (float)kScreenW + 40 + i * kPipeSpacing;
            pipe_gap_y_[i] = random_gap_y();
            pipe_scored_[i] = 0;
        }
    }

    void die()
    {
        dead_ = true;
        checkpoint_progress();
        refresh_text();
    }

    void persist_high_score()
    {
        if (hi_ <= saved_hi_) return;
        hal_config_set_int("flappy", "high_score", hi_);
        saved_hi_ = hi_;
    }

    enum class OverlayState : uint8_t {
        Invalid,
        Hidden,
        Waiting,
        Dead,
    };

    OverlayState overlay_state() const
    {
        if (dead_) return OverlayState::Dead;
        if (waiting_) return OverlayState::Waiting;
        return OverlayState::Hidden;
    }

    void refresh_text()
    {
        if (score_ && (score_val_ != rendered_score_ || hi_ != rendered_hi_)) {
            lv_label_set_text_fmt(score_, "%d  (hi %d)", score_val_, hi_);
            rendered_score_ = score_val_;
            rendered_hi_ = hi_;
        }

        const OverlayState state = overlay_state();
        if (overlay_ && state != rendered_overlay_) {
            switch (state) {
                case OverlayState::Dead:
                    lv_label_set_text(overlay_, "GAME OVER  A to retry");
                    break;
                case OverlayState::Waiting:
                    lv_label_set_text(overlay_, "A to flap");
                    break;
                default:
                    lv_label_set_text(overlay_, "");
                    break;
            }
            rendered_overlay_ = state;
        }
    }

    static void playfield_draw_event(lv_event_t *event)
    {
        auto *app = static_cast<FlappyApp *>(lv_event_get_user_data(event));
        if (app) app->draw_playfield(lv_event_get_layer(event));
    }

    static void draw_rect(lv_layer_t *layer, int x, int y, int width,
                          int height, uint32_t color, int radius = 0)
    {
        if (width <= 0 || height <= 0) return;
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_opa = LV_OPA_COVER;
        dsc.bg_color = lv_color_hex(color);
        dsc.radius = radius;
        lv_area_t area = {x, y, x + width - 1, y + height - 1};
        lv_draw_rect(layer, &dsc, &area);
    }

    void draw_playfield(lv_layer_t *layer)
    {
        draw_rect(layer, 0, kGroundY, kScreenW, kGroundH, 0x8a5a3a);
        for (int i = 0; i < kPipeCount; ++i) {
            const int px = static_cast<int>(pipe_x_[i]);
            const int py = pipe_gap_y_[i];
            draw_rect(layer, px, kTopGuard, kPipeW, py - kTopGuard,
                      0x00ffa8, 2);
            const int bottom_y = py + kPipeGap;
            draw_rect(layer, px, bottom_y, kPipeW, kGroundY - bottom_y,
                      0x00ffa8, 2);
        }
        draw_rect(layer, kBirdX, static_cast<int>(bird_y_), kBirdSize,
                  kBirdSize, 0xffd166, LV_RADIUS_CIRCLE);
    }

    void invalidate_rect(int x, int y, int width, int height)
    {
        if (!playfield_ || width <= 0 || height <= 0) return;
        lv_area_t area = {x - 1, y - 1, x + width, y + height};
        lv_obj_invalidate_area(playfield_, &area);
    }

    void invalidate_rect_pair(int old_x, int old_y, int old_width,
                              int old_height, int new_x, int new_y,
                              int new_width, int new_height)
    {
        if (old_width <= 0 || old_height <= 0) {
            invalidate_rect(new_x, new_y, new_width, new_height);
            return;
        }
        if (new_width <= 0 || new_height <= 0) {
            invalidate_rect(old_x, old_y, old_width, old_height);
            return;
        }

        const int left = old_x < new_x ? old_x : new_x;
        const int top = old_y < new_y ? old_y : new_y;
        const int old_right = old_x + old_width;
        const int new_right = new_x + new_width;
        const int right = old_right > new_right ? old_right : new_right;
        const int old_bottom = old_y + old_height;
        const int new_bottom = new_y + new_height;
        const int bottom = old_bottom > new_bottom ? old_bottom : new_bottom;
        invalidate_rect(left, top, right - left, bottom - top);
    }

    void invalidate_motion(float old_bird_y, const float *old_pipe_x,
                           const int *old_pipe_gap_y)
    {
        invalidate_rect_pair(kBirdX, static_cast<int>(old_bird_y),
                             kBirdSize, kBirdSize,
                             kBirdX, static_cast<int>(bird_y_),
                             kBirdSize, kBirdSize);

        for (int i = 0; i < kPipeCount; ++i) {
            const int old_x = static_cast<int>(old_pipe_x[i]);
            const int new_x = static_cast<int>(pipe_x_[i]);
            const int old_gap = old_pipe_gap_y[i];
            const int new_gap = pipe_gap_y_[i];

            // A wrapped pipe jumps across the screen. Keep its old and new
            // dirty areas separate so the jump does not invalidate the full
            // display width.
            if (old_gap != new_gap || new_x - old_x > kPipeW) {
                invalidate_rect(old_x, kTopGuard, kPipeW,
                                old_gap - kTopGuard);
                invalidate_rect(old_x, old_gap + kPipeGap, kPipeW,
                                kGroundY - old_gap - kPipeGap);
                invalidate_rect(new_x, kTopGuard, kPipeW,
                                new_gap - kTopGuard);
                invalidate_rect(new_x, new_gap + kPipeGap, kPipeW,
                                kGroundY - new_gap - kPipeGap);
                continue;
            }

            invalidate_rect_pair(old_x, kTopGuard, kPipeW,
                                 old_gap - kTopGuard,
                                 new_x, kTopGuard, kPipeW,
                                 new_gap - kTopGuard);
            invalidate_rect_pair(old_x, old_gap + kPipeGap, kPipeW,
                                 kGroundY - old_gap - kPipeGap,
                                 new_x, new_gap + kPipeGap, kPipeW,
                                 kGroundY - new_gap - kPipeGap);
        }
    }

    void render_full()
    {
        if (playfield_) lv_obj_invalidate(playfield_);
        refresh_text();
    }

    lv_obj_t *playfield_ = nullptr;
    lv_obj_t *score_ = nullptr;
    lv_obj_t *overlay_ = nullptr;

    float bird_y_ = 0;
    float vy_ = 0;
    float pipe_x_[kPipeCount] = {};
    int   pipe_gap_y_[kPipeCount] = {};
    uint8_t pipe_scored_[kPipeCount] = {};
    bool dead_ = false;
    bool waiting_ = true;
    int score_val_ = 0;
    int hi_ = 0;
    int saved_hi_ = 0;
    int rendered_score_ = -1;
    int rendered_hi_ = -1;
    OverlayState rendered_overlay_ = OverlayState::Invalid;
    GamePauseController pause_;
};

static FlappyApp s_instance;
App *app_flappy_instance() { return &s_instance; }
