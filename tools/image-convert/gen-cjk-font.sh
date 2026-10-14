#!/usr/bin/env bash
#
# Generate the tiny CJK subset font used by the factory test (main/test.cpp).
#
# lv_font_conv (run via npx) rasterizes ONLY the glyphs listed in SYMBOLS below
# into an LVGL C font, so the result is a few KB instead of the ~1.4 MB full
# CJK font. Re-run this whenever the Chinese strings in main/test.cpp change;
# keep SYMBOLS in sync (a missing glyph renders blank on the badge).
#
# Source font: a SINGLE-FACE .ttf/.otf (lv_font_conv cannot read macOS .ttc
# collections). Defaults to Noto Sans SC (OFL) placed next to this script.
# Override with: SRC_FONT=/path/to/font.otf ./gen-cjk-font.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_FONT="${SRC_FONT:-$HERE/NotoSansSC-Regular.otf}"
OUT="$HERE/../../main/apps/font_cjk_generated.c"
SIZE="${SIZE:-18}"

# Every glyph rendered by main/test.cpp. CJK chars + the Latin A/B that appear
# inside the bilingual instruction strings (e.g. "灯光正常按A 故障按B").
SYMBOLS='按下所有钮红绿蓝白灯光正常故障扫描任意卡片或跳过测试完成加速度计重新AB'

if [ ! -f "$SRC_FONT" ]; then
  echo "ERROR: source font not found: $SRC_FONT" >&2
  echo "Place a single-face CJK .ttf/.otf there (e.g. Noto Sans SC, OFL)," >&2
  echo "or pass SRC_FONT=/path/to/font.otf" >&2
  exit 1
fi

echo "Generating $OUT from $SRC_FONT (size=$SIZE)"
echo "symbols ($(echo -n "$SYMBOLS" | wc -m | tr -d ' ') glyphs): $SYMBOLS"

npx --yes lv_font_conv@latest \
  --font "$SRC_FONT" \
  --size "$SIZE" \
  --bpp 4 \
  --format lvgl \
  --no-compress \
  --lv-font-name font_cjk_18 \
  --symbols "$SYMBOLS" \
  -o "$OUT"

echo "Done: $OUT"
