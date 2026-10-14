#include "hal_screencap.h"
#include "hal_display.h"
#include "hal_sleep.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

// Screen capture without a framebuffer.
//
// The badge is an ESP32-C3 with no PSRAM. A full frame is 320*240*2 = 150 KB
// contiguous, which is more than the whole DRAM heap once BLE, LittleFS and
// LVGL's 64 KB pool are up — and LVGL is configured for PARTIAL render mode, so
// no complete frame exists in RAM at any instant anyway. The ST7789 sits on a
// no-MISO SPI bus, so its GRAM can't be read back either.
//
// So instead of copying a frame, we ride the one that LVGL is already drawing:
// force a full-screen invalidate and encode each 320x40 stripe to stdout as it
// is handed to the flush callback. lv_refr_now() refreshes synchronously on the
// calling task, so the whole capture happens on the console task's stack with
// no accumulation buffer and no cross-task handoff.
//
// Everything on the wire is ASCII. The console VFS is set to
// ESP_LINE_ENDINGS_CRLF on TX, so any raw 0x0A in a binary payload would come
// out as 0x0D 0x0A and corrupt the image; base64's alphabet avoids that.

namespace {

constexpr uint8_t kB64Cols = 76;

// Longest run and longest literal a control byte can describe.
constexpr uint16_t kMaxRun = 129;   // 0x80 + (n - 2), so n in [2, 129]
constexpr uint16_t kMaxLit = 128;   // 0x00 + (n - 1), so n in [1, 128]

// Yield this often so a slow host stalls the transfer instead of starving the
// idle task. TWDT is 5 s with panic disabled, but there's no reason to lean on
// that.
constexpr uint32_t kLinesPerYield = 64;

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// ---- base64, streaming ----------------------------------------------------

struct B64Out {
    uint8_t  carry[3];
    uint8_t  carry_n;
    char     line[kB64Cols + 1];
    uint8_t  line_n;
    uint32_t wire_bytes;
    uint32_t lines;
};

void b64_reset(B64Out &o)
{
    o.carry_n = 0;
    o.line_n = 0;
}

void b64_flush_line(B64Out &o)
{
    if (o.line_n == 0) return;
    o.line[o.line_n] = 0;
    fputs(o.line, stdout);
    fputc('\n', stdout);
    o.line_n = 0;
    if (++o.lines % kLinesPerYield == 0) vTaskDelay(0);
}

void b64_putc(B64Out &o, char c)
{
    o.line[o.line_n++] = c;
    o.wire_bytes++;
    if (o.line_n >= kB64Cols) b64_flush_line(o);
}

// n is 1..3; short groups get '=' padding, which only happens at stripe end.
void b64_group(B64Out &o, const uint8_t *b, uint8_t n)
{
    uint32_t v = (uint32_t)b[0] << 16;
    if (n > 1) v |= (uint32_t)b[1] << 8;
    if (n > 2) v |= (uint32_t)b[2];
    b64_putc(o, kB64[(v >> 18) & 63]);
    b64_putc(o, kB64[(v >> 12) & 63]);
    b64_putc(o, n > 1 ? kB64[(v >> 6) & 63] : '=');
    b64_putc(o, n > 2 ? kB64[v & 63] : '=');
}

void b64_put(B64Out &o, uint8_t byte)
{
    o.carry[o.carry_n++] = byte;
    if (o.carry_n == 3) {
        b64_group(o, o.carry, 3);
        o.carry_n = 0;
    }
}

// Pad and terminate. Each stripe is independently base64-framed so the host can
// decode one stripe at a time without buffering the whole image.
void b64_end(B64Out &o)
{
    if (o.carry_n) {
        b64_group(o, o.carry, o.carry_n);
        o.carry_n = 0;
    }
    b64_flush_line(o);
}

// ---- rle1, streaming ------------------------------------------------------
//
// PackBits over 16-bit pixels. Control byte:
//   0x00..0x7F  -> (n + 1) literal pixels follow, 2 bytes each, little-endian
//   0x80..0xFF  -> run of (n - 0x80 + 2) copies of the single pixel that follows
//
// Worst case is one extra byte per 128 pixels (~0.4%). Flat UI backgrounds
// collapse by orders of magnitude, which is the case that matters — this exists
// so a screenshot is a fraction of a second rather than 200 KB of base64.

struct RleOut {
    B64Out  *out;
    uint16_t lit[kMaxLit];
    uint16_t lit_n;
    uint16_t run_px;
    uint16_t run_n;     // 0 = no run in progress
};

void rle_byte(RleOut &r, uint8_t b)
{
    b64_put(*r.out, b);
}

void rle_px(RleOut &r, uint16_t px)
{
    rle_byte(r, (uint8_t)(px & 0xFF));
    rle_byte(r, (uint8_t)(px >> 8));
}

void rle_flush_lit(RleOut &r)
{
    if (r.lit_n == 0) return;
    rle_byte(r, (uint8_t)(r.lit_n - 1));
    for (uint16_t i = 0; i < r.lit_n; i++) rle_px(r, r.lit[i]);
    r.lit_n = 0;
}

// Close out the pending run: two or more identical pixels earn a run code,
// a lone one just joins the literal buffer.
void rle_retire_run(RleOut &r)
{
    if (r.run_n == 0) return;
    if (r.run_n >= 2) {
        rle_flush_lit(r);
        rle_byte(r, (uint8_t)(0x80 + (r.run_n - 2)));
        rle_px(r, r.run_px);
    } else {
        r.lit[r.lit_n++] = r.run_px;
        if (r.lit_n == kMaxLit) rle_flush_lit(r);
    }
    r.run_n = 0;
}

void rle_begin(RleOut &r, B64Out *out)
{
    r.out = out;
    r.lit_n = 0;
    r.run_n = 0;
}

void rle_put(RleOut &r, uint16_t px)
{
    if (r.run_n > 0 && px == r.run_px && r.run_n < kMaxRun) {
        r.run_n++;
        return;
    }
    rle_retire_run(r);
    r.run_px = px;
    r.run_n = 1;
}

void rle_end(RleOut &r)
{
    rle_retire_run(r);
    rle_flush_lit(r);
}

// ---- capture --------------------------------------------------------------

struct CapCtx {
    lv_display_t   *disp;
    ScreenCapStats *st;
    uint32_t        crc;
    B64Out          b64;
    RleOut          rle;
};

// One capture at a time (the LVGL lock serialises it, and the console is
// single-threaded). Static rather than stack because the REPL task gets 4 KB
// and this is ~400 bytes on top of whatever printf wants.
CapCtx s_ctx;

void on_flush_start(lv_event_t *e)
{
    CapCtx *c = (CapCtx *)lv_event_get_user_data(e);
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    if (!c || !a) return;

    // lv_refr.c sets layer->draw_buf = disp->buf_act before rendering and only
    // swaps buffers *after* the flush callback returns, so the active buffer is
    // the one being flushed right now.
    lv_draw_buf_t *db = lv_display_get_buf_active(c->disp);
    if (!db || !db->data) return;

    const int32_t w = lv_area_get_width(a);
    const int32_t h = lv_area_get_height(a);
    const uint32_t stride = db->header.stride;
    if (w <= 0 || h <= 0 || stride < (uint32_t)w * 2) return;

    // Per-stripe area, so the host composites by coordinate rather than
    // trusting LVGL to split the screen top-to-bottom.
    printf("S %d %d %d %d\n", (int)a->x1, (int)a->y1, (int)a->x2, (int)a->y2);

    b64_reset(c->b64);
    rle_begin(c->rle, &c->b64);

    for (int32_t y = 0; y < h; y++) {
        const uint8_t *row = db->data + (size_t)y * stride;
        // CRC covers the decoded RGB565 stream in emission order, seed 0 /
        // zlib convention — same as fs_snapshot and bbx_proto.
        c->crc = esp_rom_crc32_le(c->crc, row, (uint32_t)w * 2);
        const uint16_t *px = (const uint16_t *)(const void *)row;
        for (int32_t x = 0; x < w; x++) rle_put(c->rle, px[x]);
    }

    rle_end(c->rle);
    b64_end(c->b64);

    c->st->stripes++;
    c->st->pixels_covered += (uint32_t)w * (uint32_t)h;
    c->st->raw_bytes += (uint32_t)w * (uint32_t)h * 2;
}

}  // namespace

bool hal_screencap_stream(ScreenCapStats *out)
{
    lv_display_t *disp = hal_display_lv();
    if (!disp) return false;

    // A capture means a host is driving this badge. Light sleep would suspend
    // USB-Serial-JTAG mid-transfer and the host would just see a timeout.
    hal_sleep_note_activity();

    ScreenCapStats st = {};
    st.w = (uint16_t)lv_display_get_horizontal_resolution(disp);
    st.h = (uint16_t)lv_display_get_vertical_resolution(disp);

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.disp = disp;
    s_ctx.st = &st;

    printf("SHOT %u %u rgb565le rle1\n", (unsigned)st.w, (unsigned)st.h);
    fflush(stdout);

    const int64_t t0 = esp_timer_get_time();

    if (!lvgl_port_lock(0)) return false;
    lv_display_add_event_cb(disp, on_flush_start, LV_EVENT_FLUSH_START, &s_ctx);
    // Area-based invalidation, so the redraw composites lv_layer_top() too —
    // the boot splash and the sleep card live there, not under
    // hal_display_root(), and a capture that missed them would be a lie.
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    lv_display_remove_event_cb_with_user_data(disp, on_flush_start, &s_ctx);
    lvgl_port_unlock();

    st.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    st.crc32 = s_ctx.crc;
    st.wire_bytes = s_ctx.b64.wire_bytes;
    st.complete = (st.pixels_covered == (uint32_t)st.w * (uint32_t)st.h);

    printf("END bytes=%u raw=%u crc32=%08lx stripes=%u covered=%u elapsed_ms=%u\n",
           (unsigned)st.wire_bytes, (unsigned)st.raw_bytes,
           (unsigned long)st.crc32, (unsigned)st.stripes,
           (unsigned)st.pixels_covered, (unsigned)st.elapsed_ms);

    if (out) *out = st;
    return true;
}
