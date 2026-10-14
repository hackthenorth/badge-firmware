#include "hal_uitree.h"
#include "hal_display.h"
#include "json_util.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"

// lv_obj_class_t::name lives in the private header — LVGL has no public
// accessor for it. Worth the private include: the alternative is a hand-kept
// table of lv_obj_check_type() calls that silently prints "?" for every widget
// class someone adds later.
#include "core/lv_obj_class_private.h"

#include <cstdio>
#include <cstring>

namespace {

// The REPL task gets 4 KB of stack, so the walk is iterative over an explicit
// stack — same reason fs_snapshot.cpp walks the filesystem that way.
constexpr uint8_t  kMaxDepth = 10;
constexpr uint32_t kMaxNodes = 400;

// Long enough to identify a widget, short enough that a runaway string can't
// flood the console. json_escape() expands a control char to six bytes.
constexpr size_t kMaxText = 64;
constexpr size_t kTextBuf = kMaxText * 6 + 1;

struct Level {
    lv_obj_t *obj;
    uint32_t  idx;     // next child to visit
    uint32_t  count;
};

struct StateName { lv_state_t bit; const char *name; };

const StateName kStates[] = {
    { LV_STATE_CHECKED,  "CHECKED"  },
    { LV_STATE_FOCUSED,  "FOCUSED"  },
    { LV_STATE_FOCUS_KEY,"FOCUS_KEY"},
    { LV_STATE_EDITED,   "EDITED"   },
    { LV_STATE_HOVERED,  "HOVERED"  },
    { LV_STATE_PRESSED,  "PRESSED"  },
    { LV_STATE_SCROLLED, "SCROLLED" },
    { LV_STATE_DISABLED, "DISABLED" },
};

void print_node(lv_obj_t *obj, uint8_t depth, int32_t idx)
{
    for (uint8_t i = 0; i < depth; i++) fputs("  ", stdout);
    if (idx >= 0) printf("[%d] ", (int)idx);

    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    const char *name = (cls && cls->name) ? cls->name : "?";

    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    printf("%s %d,%d %dx%d", name, (int)c.x1, (int)c.y1,
           (int)lv_area_get_width(&c), (int)lv_area_get_height(&c));

    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) fputs(" hidden", stdout);

    lv_state_t st = lv_obj_get_state(obj);
    bool first = true;
    for (const auto &s : kStates) {
        if (!(st & s.bit)) continue;
        fputs(first ? " state=" : "|", stdout);
        fputs(s.name, stdout);
        first = false;
    }

    // Label text is the single most useful field here — it's what turns a tree
    // of anonymous containers into something you can assert against.
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char *text = lv_label_get_text(obj);
        if (text) {
            // Cut the source at a codepoint boundary *before* escaping.
            // Truncating the escaped form instead could slice a \u00XX
            // sequence in half.
            size_t n = strlen(text);
            if (n > kMaxText) n = utf8_valid_prefix((const uint8_t *)text, kMaxText);
            char src[kMaxText + 1];
            memcpy(src, text, n);
            src[n] = 0;

            char esc[kTextBuf];
            json_escape(esc, sizeof(esc), src);
            printf(" text=\"%s\"", esc);
        }
    }

    fputc('\n', stdout);
}

void dump_subtree(lv_obj_t *root, UiTreeStats *st)
{
    if (!root) return;

    print_node(root, 1, -1);
    st->nodes++;

    Level stack[kMaxDepth];
    uint8_t sp = 0;
    stack[0] = { root, 0, lv_obj_get_child_count(root) };

    while (true) {
        Level &lvl = stack[sp];
        if (lvl.idx >= lvl.count) {
            if (sp == 0) return;
            sp--;
            continue;
        }
        lv_obj_t *child = lv_obj_get_child(lvl.obj, lvl.idx);
        const uint32_t child_idx = lvl.idx;
        lvl.idx++;
        if (!child) continue;

        const uint8_t depth = sp + 1;
        print_node(child, depth + 1, (int32_t)child_idx);
        st->nodes++;
        if (depth > st->max_depth) st->max_depth = depth;

        if (st->nodes >= kMaxNodes) { st->truncated = true; return; }

        const uint32_t grandkids = lv_obj_get_child_count(child);
        if (grandkids == 0) continue;
        if (sp + 1 >= kMaxDepth) { st->truncated = true; continue; }

        sp++;
        stack[sp] = { child, 0, grandkids };
    }
}

}  // namespace

bool hal_uitree_dump(UiTreeStats *out)
{
    lv_display_t *disp = hal_display_lv();
    if (!disp) return false;

    UiTreeStats st = {};

    if (!lvgl_port_lock(0)) return false;

    printf("UITREE\n");
    printf("screen %dx%d\n",
           (int)lv_display_get_horizontal_resolution(disp),
           (int)lv_display_get_vertical_resolution(disp));
    dump_subtree(lv_screen_active(), &st);

    // The boot splash and the sleep card are parented here, not under the
    // active screen, so a dump that stopped at the screen would miss whatever
    // is actually covering it.
    printf("top\n");
    lv_obj_t *top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) == 0) {
        printf("  (empty)\n");
    } else {
        dump_subtree(top, &st);
    }

    lvgl_port_unlock();

    printf("END nodes=%u depth=%u truncated=%s\n",
           (unsigned)st.nodes, (unsigned)st.max_depth,
           st.truncated ? "yes" : "no");

    if (out) *out = st;
    return true;
}
