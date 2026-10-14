#include "app.h"
#include "app_registry.h"
#include "hal_accel.h"

namespace {

constexpr int kAxisCount = 3;
constexpr int kChartPointCount = 40;
constexpr int kChartRangeMg = 2000;
// 25 Hz is responsive enough to track hand motion while staying well below
// the sensor's 100 Hz output rate and avoiding a redraw on every app tick.
constexpr int kSampleEveryTicks = 2; // 40 ms at the registry's 20 ms cadence
constexpr uint32_t kAxisColors[kAxisCount] = {
    0xff5555, 0x55e080, 0x55aaff,
};
constexpr const char *kAxisNames[kAxisCount] = {"X", "Y", "Z"};

} // namespace

extern "C" const lv_image_dsc_t icon_accel;

class AccelApp : public App {
public:
    const char *name() const override { return "Accel"; }
    const char *icon() const override { return "ACC"; }
    const char *slug() const override { return "accel"; }
    AppAccess access() const override { return AppAccess::Internal; }
    const lv_image_dsc_t *icon_image() const override { return &icon_accel; }

    void on_enter(lv_obj_t *root) override
    {
        // Populate the readout on the first registry tick, then use the normal
        // 40 ms cadence.
        sample_tick_ = kSampleEveryTicks - 1;

        lv_obj_t *title = lv_label_create(root);
        lv_label_set_text(title, "Accelerometer");
        lv_obj_set_style_text_color(title, lv_color_hex(BadgeTheme::TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        for (int i = 0; i < kAxisCount; ++i) {
            const int y = 34 + i * 64;

            readouts_[i] = lv_label_create(root);
            lv_obj_set_width(readouts_[i], 66);
            lv_obj_set_style_text_align(readouts_[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(readouts_[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(readouts_[i], lv_color_hex(kAxisColors[i]), 0);
            lv_obj_set_pos(readouts_[i], 3, y + 8);
            lv_label_set_text_fmt(readouts_[i], "%s\n%+d mg", kAxisNames[i], 0);

            charts_[i] = lv_chart_create(root);
            lv_obj_set_size(charts_[i], 242, 54);
            lv_obj_set_pos(charts_[i], 72, y);
            lv_chart_set_type(charts_[i], LV_CHART_TYPE_LINE);
            lv_chart_set_point_count(charts_[i], kChartPointCount);
            lv_chart_set_axis_range(charts_[i], LV_CHART_AXIS_PRIMARY_Y,
                                    -kChartRangeMg, kChartRangeMg);
            lv_chart_set_update_mode(charts_[i], LV_CHART_UPDATE_MODE_SHIFT);
            lv_chart_set_div_line_count(charts_[i], 3, 5);
            lv_obj_set_style_bg_color(charts_[i], lv_color_hex(BadgeTheme::PANEL), 0);
            lv_obj_set_style_bg_opa(charts_[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(charts_[i], lv_color_hex(BadgeTheme::BORDER), 0);
            lv_obj_set_style_border_width(charts_[i], 1, 0);
            lv_obj_set_style_radius(charts_[i], 4, 0);
            lv_obj_set_style_pad_all(charts_[i], 3, 0);
            lv_obj_set_style_line_color(charts_[i], lv_color_hex(BadgeTheme::BORDER), 0);
            lv_obj_set_style_line_opa(charts_[i], LV_OPA_50, 0);
            lv_obj_set_style_line_width(charts_[i], 2, LV_PART_ITEMS);
            lv_obj_set_style_size(charts_[i], 0, 0, LV_PART_INDICATOR);

            series_[i] = lv_chart_add_series(charts_[i],
                lv_color_hex(kAxisColors[i]), LV_CHART_AXIS_PRIMARY_Y);
        }
    }

    void on_exit() override
    {
        for (int i = 0; i < kAxisCount; ++i) {
            charts_[i] = nullptr;
            readouts_[i] = nullptr;
            series_[i] = nullptr;
        }
    }

    void on_tick() override
    {
        if (++sample_tick_ < kSampleEveryTicks) return;
        sample_tick_ = 0;

        AccelSample s;
        if (hal_accel_read(&s) != ESP_OK) return;

        const int32_t values[kAxisCount] = {
            static_cast<int32_t>(s.x_mg),
            static_cast<int32_t>(s.y_mg),
            static_cast<int32_t>(s.z_mg),
        };
        for (int i = 0; i < kAxisCount; ++i) {
            if (charts_[i] && series_[i]) {
                lv_chart_set_next_value(charts_[i], series_[i], values[i]);
            }
            if (readouts_[i]) {
                lv_label_set_text_fmt(readouts_[i], "%s\n%+ld mg",
                    kAxisNames[i], static_cast<long>(values[i]));
            }
        }
    }

private:
    lv_obj_t *charts_[kAxisCount] = {};
    lv_obj_t *readouts_[kAxisCount] = {};
    lv_chart_series_t *series_[kAxisCount] = {};
    int sample_tick_ = 0;
};

static AccelApp s_instance;
App *app_accel_instance() { return &s_instance; }
