#include "app.h"
#include "app_registry.h"

#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

static constexpr uint32_t COLOR_BG       = BadgeTheme::BACKGROUND;
static constexpr uint32_t COLOR_PANEL    = BadgeTheme::PANEL;
static constexpr uint32_t COLOR_FOCUS    = BadgeTheme::SURFACE;
static constexpr uint32_t COLOR_BORDER   = BadgeTheme::BORDER;
static constexpr uint32_t COLOR_ACCENT   = BadgeTheme::ACCENT_DETAIL;
static constexpr uint32_t COLOR_TEXT     = BadgeTheme::TEXT;
static constexpr uint32_t COLOR_TEXT_DIM = BadgeTheme::TEXT_MUTED;

static constexpr int VISIBLE_ROWS    = 9;
static constexpr int ROW_H           = 18;
static constexpr int LIST_TOP        = 28;
static constexpr int LIST_X          = 6;
static constexpr int LIST_W          = 308;
static constexpr size_t MAX_FILE_BYTES = 8 * 1024;

struct Entry {
    std::string name;
    bool is_dir;
};

extern "C" const lv_image_dsc_t icon_files;

class FilesApp : public App {
public:
    const char *name() const override { return "Files"; }
    const char *icon() const override { return LV_SYMBOL_DIRECTORY; }
    const char *slug() const override { return "files"; }
    AppAccess access() const override { return AppAccess::Internal; }
    const lv_image_dsc_t *icon_image() const override { return &icon_files; }

    void on_enter(lv_obj_t *root) override
    {
        root_ = root;
        lv_obj_set_style_bg_color(root_, lv_color_hex(COLOR_BG), 0);
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);

        path_ = "/littlefs";
        viewing_file_ = false;
        sel_ = 0;
        scroll_ = 0;

        build_list_ui();
        load_dir();
        render_list();
    }

    void on_exit() override
    {
        root_ = nullptr;
        title_ = nullptr;
        list_panel_ = nullptr;
        for (auto &row : rows_) row = nullptr;
        file_view_ = nullptr;
        file_label_ = nullptr;
        entries_.clear();
        file_text_.clear();
    }

    void on_button(ButtonEvent ev) override
    {
        if (ev.kind != ButtonEventKind::Pressed) return;
        if (viewing_file_) handle_file_button(ev);
        else handle_list_button(ev);
    }

private:
    void handle_list_button(ButtonEvent ev)
    {
        switch (ev.button) {
            case Button::Up:
                if (sel_ > 0) { sel_--; ensure_visible(); render_list(); }
                break;
            case Button::Down:
                if (sel_ + 1 < (int)entries_.size()) { sel_++; ensure_visible(); render_list(); }
                break;
            case Button::A:
            case Button::Right:
            case Button::Start:
                activate_selected();
                break;
            case Button::Left:
                go_up();
                break;
            default: break;
        }
    }

    void handle_file_button(ButtonEvent ev)
    {
        if (!file_view_) return;
        switch (ev.button) {
            case Button::Up:
                lv_obj_scroll_by(file_view_, 0, ROW_H * 3, LV_ANIM_OFF);
                break;
            case Button::Down:
                lv_obj_scroll_by(file_view_, 0, -ROW_H * 3, LV_ANIM_OFF);
                break;
            case Button::Left:
            case Button::A:
                close_file();
                break;
            default: break;
        }
    }

    void build_list_ui()
    {
        title_ = lv_label_create(root_);
        lv_obj_set_style_text_font(title_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(title_, lv_color_hex(COLOR_ACCENT), 0);
        lv_obj_set_pos(title_, LIST_X, 6);
        lv_obj_set_width(title_, LIST_W);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_DOT);

        list_panel_ = lv_obj_create(root_);
        lv_obj_remove_style_all(list_panel_);
        lv_obj_set_pos(list_panel_, LIST_X, LIST_TOP);
        lv_obj_set_size(list_panel_, LIST_W, VISIBLE_ROWS * ROW_H + 8);
        lv_obj_set_style_bg_color(list_panel_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(list_panel_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(list_panel_, 1, 0);
        lv_obj_set_style_border_color(list_panel_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_radius(list_panel_, 0, 0);
        lv_obj_set_style_pad_all(list_panel_, 4, 0);
        lv_obj_clear_flag(list_panel_, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < VISIBLE_ROWS; i++) {
            lv_obj_t *row = lv_label_create(list_panel_);
            lv_obj_set_style_text_font(row, &lv_font_montserrat_14, 0);
            lv_obj_set_pos(row, 0, i * ROW_H);
            lv_obj_set_width(row, LIST_W - 8);
            lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
            rows_[i] = row;
        }
    }

    void load_dir()
    {
        entries_.clear();
        sel_ = 0;
        scroll_ = 0;

        DIR *d = opendir(path_.c_str());
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.' && (de->d_name[1] == '\0'
                || (de->d_name[1] == '.' && de->d_name[2] == '\0'))) continue;
            Entry e;
            e.name = de->d_name;
            if (de->d_type == DT_UNKNOWN) {
                std::string full = path_ + "/" + e.name;
                struct stat st;
                e.is_dir = (stat(full.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
            } else {
                e.is_dir = (de->d_type == DT_DIR);
            }
            entries_.push_back(std::move(e));
        }
        closedir(d);

        std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
            if (a.is_dir != b.is_dir) return a.is_dir;
            return a.name < b.name;
        });
    }

    void ensure_visible()
    {
        if (sel_ < scroll_) scroll_ = sel_;
        else if (sel_ >= scroll_ + VISIBLE_ROWS) scroll_ = sel_ - VISIBLE_ROWS + 1;
    }

    void render_list()
    {
        if (title_) lv_label_set_text(title_, path_.c_str());

        for (int i = 0; i < VISIBLE_ROWS; i++) {
            lv_obj_t *row = rows_[i];
            int idx = scroll_ + i;
            if (idx >= (int)entries_.size()) {
                lv_label_set_text(row, "");
                lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
                continue;
            }
            const Entry &e = entries_[idx];
            const char *sym = e.is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            char buf[256];
            snprintf(buf, sizeof(buf), " %s  %s%s", sym, e.name.c_str(), e.is_dir ? "/" : "");
            lv_label_set_text(row, buf);

            bool focused = (idx == sel_);
            lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_FOCUS), 0);
            lv_obj_set_style_bg_opa(row, focused ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_radius(row, 0, 0);
            lv_obj_set_style_text_color(row,
                lv_color_hex(focused ? COLOR_ACCENT : (e.is_dir ? COLOR_TEXT : COLOR_TEXT_DIM)), 0);
        }

        if (entries_.empty()) {
            lv_label_set_text(rows_[0], " (empty)");
            lv_obj_set_style_text_color(rows_[0], lv_color_hex(COLOR_TEXT_DIM), 0);
        }
    }

    void activate_selected()
    {
        if (sel_ < 0 || sel_ >= (int)entries_.size()) return;
        const Entry &e = entries_[sel_];
        std::string next = path_ + "/" + e.name;
        if (e.is_dir) {
            path_ = std::move(next);
            load_dir();
            render_list();
        } else {
            open_file(next);
        }
    }

    void go_up()
    {
        if (path_ == "/littlefs" || path_ == "/") return;
        size_t slash = path_.find_last_of('/');
        if (slash == std::string::npos || slash == 0) path_ = "/";
        else path_ = path_.substr(0, slash);
        load_dir();
        render_list();
    }

    void open_file(const std::string &full_path)
    {
        FILE *f = fopen(full_path.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz < 0) sz = 0;
        size_t to_read = (size_t)sz;
        bool truncated = false;
        if (to_read > MAX_FILE_BYTES) { to_read = MAX_FILE_BYTES; truncated = true; }

        file_text_.assign(to_read, '\0');
        if (to_read > 0) fread(file_text_.data(), 1, to_read, f);
        fclose(f);
        if (truncated) file_text_ += "\n\n[truncated]";

        for (char &c : file_text_) {
            if ((unsigned char)c < 9 || (c > 13 && (unsigned char)c < 32)) c = '.';
        }

        viewing_file_ = true;
        if (list_panel_) lv_obj_add_flag(list_panel_, LV_OBJ_FLAG_HIDDEN);

        const char *fname = full_path.c_str();
        const char *slash = strrchr(fname, '/');
        if (slash) fname = slash + 1;
        char title_buf[160];
        snprintf(title_buf, sizeof(title_buf), LV_SYMBOL_FILE "  %s", fname);
        lv_label_set_text(title_, title_buf);

        file_view_ = lv_obj_create(root_);
        lv_obj_remove_style_all(file_view_);
        lv_obj_set_pos(file_view_, LIST_X, LIST_TOP);
        lv_obj_set_size(file_view_, LIST_W, 240 - LIST_TOP - 6);
        lv_obj_set_style_bg_color(file_view_, lv_color_hex(COLOR_PANEL), 0);
        lv_obj_set_style_bg_opa(file_view_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(file_view_, 1, 0);
        lv_obj_set_style_border_color(file_view_, lv_color_hex(COLOR_BORDER), 0);
        lv_obj_set_style_radius(file_view_, 0, 0);
        lv_obj_set_style_pad_all(file_view_, 6, 0);
        lv_obj_set_scroll_dir(file_view_, LV_DIR_VER);

        file_label_ = lv_label_create(file_view_);
        lv_obj_set_style_text_font(file_label_, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(file_label_, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_width(file_label_, LIST_W - 16);
        lv_label_set_long_mode(file_label_, LV_LABEL_LONG_WRAP);
        lv_label_set_text(file_label_, file_text_.empty() ? "(empty)" : file_text_.c_str());
    }

    void close_file()
    {
        viewing_file_ = false;
        if (file_view_) {
            lv_obj_del(file_view_);
            file_view_ = nullptr;
            file_label_ = nullptr;
        }
        file_text_.clear();
        if (list_panel_) lv_obj_clear_flag(list_panel_, LV_OBJ_FLAG_HIDDEN);
        render_list();
    }

    lv_obj_t *root_ = nullptr;
    lv_obj_t *title_ = nullptr;
    lv_obj_t *list_panel_ = nullptr;
    lv_obj_t *rows_[VISIBLE_ROWS] = {};
    lv_obj_t *file_view_ = nullptr;
    lv_obj_t *file_label_ = nullptr;

    std::string path_;
    std::vector<Entry> entries_;
    int sel_ = 0;
    int scroll_ = 0;

    bool viewing_file_ = false;
    std::string file_text_;
};

static FilesApp s_instance;
App *app_files_instance() { return &s_instance; }
