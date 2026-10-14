#!/usr/bin/env python3
"""Crop, resize, and encode the Sleeping Patch artwork for LVGL."""

from pathlib import Path

try:
    from PIL import Image
except ImportError:
    raise SystemExit("Pillow is required: pip install pillow")


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "assets" / "sleeping" / "sleeping_patch.png"
OUTPUT = ROOT / "main" / "apps" / "icons_generated" / "sleeping_patch.c"
MAX_SIZE = (124, 106)
SYMBOL = "sleeping_patch"


def c_bytes(data: bytes) -> str:
    rows = []
    for offset in range(0, len(data), 16):
        chunk = data[offset:offset + 16]
        rows.append("    " + ",".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(rows)


image = Image.open(SOURCE).convert("RGBA")
bounds = image.getchannel("A").getbbox()
if bounds is None:
    raise SystemExit(f"artwork has no visible pixels: {SOURCE}")

image = image.crop(bounds)
image.thumbnail(MAX_SIZE, Image.Resampling.LANCZOS)
width, height = image.size

rgb565 = bytearray()
alpha = bytearray()
for red, green, blue, opacity in image.getdata():
    color = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
    rgb565.extend((color & 0xFF, color >> 8))
    alpha.append(opacity)

payload = bytes(rgb565 + alpha)
source = f'''#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)
#include <lvgl.h>
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_SLEEPING_PATCH
#define LV_ATTRIBUTE_SLEEPING_PATCH
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_SLEEPING_PATCH
uint8_t {SYMBOL}_map[] = {{
{c_bytes(payload)}
}};

const lv_image_dsc_t {SYMBOL} = {{
  .header = {{
    .magic = LV_IMAGE_HEADER_MAGIC,
    .cf = LV_COLOR_FORMAT_RGB565A8,
    .flags = 0,
    .w = {width},
    .h = {height},
    .stride = {width * 2},
    .reserved_2 = 0,
  }},
  .data_size = sizeof({SYMBOL}_map),
  .data = {SYMBOL}_map,
  .reserved = NULL,
}};
'''

OUTPUT.parent.mkdir(parents=True, exist_ok=True)
OUTPUT.write_text(source)
print(f"generated {OUTPUT.relative_to(ROOT)}: {width}x{height}, {len(payload)} bytes")
