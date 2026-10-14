#!/usr/bin/env bash
#
# Generate the tiny chess-piece subset font used by app_chess.
#
# Modelled on gen-cjk-font.sh: lv_font_conv rasterizes ONLY the listed glyphs
# into an LVGL C font, so the result is a couple of KB rather than a whole
# symbol face.
#
# THREE THINGS THAT ARE EASY TO GET WRONG HERE, all of which fail silently:
#
#   1. Only the SOLID glyphs (U+265A-265F) are rendered, and both colours are
#      drawn from them, distinguished by text colour. The outline "white" set
#      (U+2654-2659) is nearly invisible against this badge's dark board, and
#      rendering it would double the font for a worse result.
#
#   2. --range, not --symbols. Twelve literal chess characters in a shell
#      script survive exactly as long as nobody opens the file in an editor
#      that helpfully normalises something.
#
#   3. NOT every font has these glyphs, and Noto Sans SC (what gen-cjk-font.sh
#      uses) does NOT. lv_font_conv does not fail in that case — it emits a
#      font without them, and the badge renders the placeholder box. So this
#      script greps its own output and fails loudly if the glyphs are missing.
#
# Source font: a SINGLE-FACE .ttf/.otf (lv_font_conv cannot read macOS .ttc
# collections) that actually contains U+265A-265F. Noto Sans Symbols 2 (OFL)
# and DejaVu Sans (Bitstream Vera license) both do. Place it next to this
# script; it is gitignored, because only the generated .c is committed.
#
#   SRC_FONT=/path/to/font.ttf ./gen-chess-font.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_FONT="${SRC_FONT:-$HERE/NotoSansSymbols2-Regular.ttf}"
OUT="$HERE/../../main/apps/font_chess_generated.c"

# 20, not 24. The board cell is 24 px and has to carry a 2 px cursor border on
# top of the glyph; a 24 px face leaves the piece touching the highlight.
SIZE="${SIZE:-20}"

# U+265A KING .. U+265F PAWN (solid forms), plus U+2022 BULLET.
#
# The bullet is not decoration: app_chess marks legal destinations by putting
# it in the cell's own label, and a cell label renders in THIS font. Leave it
# out and every legal-move marker becomes a placeholder box.
RANGE="0x265A-0x265F,0x2022"

if [ ! -f "$SRC_FONT" ]; then
  echo "ERROR: source font not found: $SRC_FONT" >&2
  echo "" >&2
  echo "Place a single-face .ttf/.otf containing U+265A-265F there. Both of" >&2
  echo "these are permissively licensed and work:" >&2
  echo "  Noto Sans Symbols 2  https://fonts.google.com/noto/specimen/Noto+Sans+Symbols+2" >&2
  echo "  DejaVu Sans          https://dejavu-fonts.github.io/" >&2
  echo "" >&2
  echo "Or pass SRC_FONT=/path/to/font.ttf" >&2
  exit 1
fi

echo "Generating $OUT from $SRC_FONT (size=$SIZE, range=$RANGE)"

npx --yes lv_font_conv@latest \
  --font "$SRC_FONT" \
  --size "$SIZE" \
  --bpp 4 \
  --format lvgl \
  --no-compress \
  --lv-font-name font_chess_20 \
  --range "$RANGE" \
  -o "$OUT"

# lv_font_conv happily emits a font missing every glyph you asked for when the
# source does not have them. Six codepoints in, six out, or this is not a
# usable font and app_chess would silently render placeholder boxes.
missing=0
for cp in 265A 265B 265C 265D 265E 265F 2022; do
  if ! grep -qi "U+$cp" "$OUT"; then
    echo "ERROR: $SRC_FONT has no glyph for U+$cp" >&2
    missing=1
  fi
done
if [ "$missing" -ne 0 ]; then
  echo "" >&2
  echo "The generated file is unusable; pick a source font with chess glyphs." >&2
  rm -f "$OUT"
  exit 1
fi

echo "Done: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes, 7 glyphs)"
