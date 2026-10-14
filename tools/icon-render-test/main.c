#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static unsigned char buffer[96 * 96 * 2];
static unsigned char frame[96 * 96 * 2];

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *px)
{
    const int bytes = (area->x2 - area->x1 + 1) * 2;
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(frame + (y * 96 + area->x1) * 2, px, bytes);
        px += bytes;
    }
    lv_display_flush_ready(display);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(96, 96);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, 255, 0);

    // The flasher uploads an opaque 42x42 thumbnail, displayed at native size.
    static uint8_t pixels[42 * 42 * 3];
    for (int i = 0; i < 42 * 42; i++) {
        pixels[i * 2 + 1] = 0xf8;
        pixels[42 * 42 * 2 + i] = 255;
    }
    lv_image_dsc_t icon = {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565A8,
            .w = 42, .h = 42, .stride = 84,
        },
        .data_size = sizeof(pixels), .data = pixels,
    };
    FILE *file = fopen("icon-render-test.bin", "wb");
    if (!file) return 2;
    fwrite(&icon.header, 1, sizeof(icon.header), file);
    fwrite(pixels, 1, sizeof(pixels), file);
    fclose(file);

    lv_obj_t *img = lv_image_create(root);
    lv_image_set_src(img, &icon);
    lv_obj_center(img);
    lv_image_set_scale(img, LV_SCALE_NONE);
    lv_refr_now(display);
    unsigned char expected[sizeof(frame)];
    memcpy(expected, frame, sizeof(frame));

    lv_image_set_src(img, "A:icon-render-test.bin");
    lv_refr_now(display);
    int different = 0, embedded_lit = 0, file_lit = 0;
    for (int i = 0; i < 96 * 96; i++) {
        different += memcmp(expected + 2 * i, frame + 2 * i, 2) != 0;
        embedded_lit += (expected[2 * i] | expected[2 * i + 1]) != 0;
        file_lit += (frame[2 * i] | frame[2 * i + 1]) != 0;
    }
    printf("RAM_LOAD=%d embedded_pixels=%d file_pixels=%d differing_pixels=%d\n",
           TEST_RAM_LOAD, embedded_lit, file_lit, different);
    remove("icon-render-test.bin");
    return embedded_lit != 1764 || different != 0;
}
