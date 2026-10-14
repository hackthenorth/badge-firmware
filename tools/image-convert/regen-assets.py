#!/usr/bin/env python3
"""Regenerate checked-in LVGL image arrays from source PNGs.

One script, one manifest. Adding artwork should be a new ASSETS entry, not a
new file in this directory — every asset that resizes a PNG and runs it through
LVGLImage.py belongs here.

    ./regen-assets.py                 # regenerate everything
    ./regen-assets.py icons           # regenerate one group
    ./regen-assets.py --check         # verify the checked-in output is current

Not covered: regen-sleeping-patch.py, which hand-rolls its C output through a
different encoder (autocrop + thumbnail) rather than LVGLImage.py. Folding it in
would rewrite its checked-in array, so it stays separate until someone wants
that diff.
"""

import argparse
import filecmp
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    raise SystemExit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).resolve().parent / "LVGLImage.py"
OUT_DIR = ROOT / "main" / "apps" / "icons_generated"
# Must mirror BadgeTheme::BACKGROUND in main/apps/ui_theme.h.
BOOT_BACKGROUND = (0x05, 0x05, 0x05)


@dataclass(frozen=True)
class Asset:
    """One group of source PNGs sharing a size and symbol-naming rule."""

    name: str            # group name, used to select on the command line
    source: str          # file, or directory to glob when `pattern` is set
    symbol: str          # C symbol; "{stem}" expands to the source filename stem
    size: tuple = ()     # exact (w, h) to resize to; empty keeps the source size
    # Preserve aspect ratio and center within `size` instead of stretching to it.
    fit: bool = False
    # Crop transparent margins before fitting artwork into its badge canvas.
    autocrop_alpha: bool = False
    # Transparent inset inside `size`, applied when `fit` is true.
    padding: int = 0
    pattern: str = ""    # glob within `source` for multi-file groups
    # Flatten transparent artwork onto this RGB colour before encoding. This is
    # required for RGB565 assets, which do not carry an alpha channel.
    background: tuple = ()
    # Optional checked-in copy of the prepared image. This keeps derived badge
    # crops reproducible from their full-resolution transparent masters.
    prepared_png: str = ""
    # RGB565A8 keeps an alpha channel, which icons and overlay artwork need.
    # Opaque full-width artwork (the boot logos) uses RGB565 and halves the
    # flash cost. Must match the .cf the consuming C code expects.
    cf: str = "RGB565A8"
    # Convert RGBA artwork to a fixed indexed palette before LVGL encoding.
    # I8 retains transparency while using one byte per pixel on the badge.
    quantize_colors: int = 0
    # LVGLImage.py leaves a trailing blank line. The icons were checked in with
    # it and there is no reason to churn 28 files; newer assets trim it so git's
    # whitespace checks stay quiet. Match whatever is already committed, or
    # --check reports the whole group stale forever.
    trim_trailing_blank: bool = False
    # New files can use clean EOFs without churning legacy output in the same
    # globbed group. Entries are source stems.
    trim_trailing_blank_stems: tuple = ()
    # Launcher masters must contain real transparent pixels, not a flattened
    # black/checkerboard backdrop that only looks like transparency in a preview.
    require_transparent_png: bool = False


ASSETS = (
    # Reuse the event's flower sticker for both the scanner view and its
    # 96px icon master. Keep this before icons so a full run updates both.
    Asset(name="scanner",
          source="assets/icons/scanner-sticker.webp",
          size=(96, 96),
          fit=True,
          symbol="scanner_sticker",
          prepared_png="assets/icons/scanner.png",
          cf="I8",
          quantize_colors=256,
          trim_trailing_blank=True),
    Asset(name="icons",
          source="assets/icons",
          pattern="*.png",
          size=(48, 48),
          symbol="icon_{stem}",
          require_transparent_png=True,
          trim_trailing_blank_stems=("workshops", "rules")),
    # In-game sprites for the arcade batch (sokoban, hopper, jetpack,
    # invaders). Authored at exact pixel size and encoded as-is so the
    # pixels stay crisp; RGB565A8 keeps the transparency.
    Asset(name="sprites",
          source="assets/sprites",
          pattern="*.png",
          symbol="{stem}",
          trim_trailing_blank=True),
    # Badge-sized boot artwork. assets/boot/*.png are the transparent,
    # full-resolution originals the website imports. The prepared badge crops
    # are flattened onto BadgeTheme::BACKGROUND (0x050505) before RGB565
    # encoding so their edges disappear into the boot splash.
    Asset(name="boot",
          source="assets/boot/htn_logo_horizontal.png",
          size=(276, 52),
          fit=True,
          symbol="boot_logo_htn",
          background=BOOT_BACKGROUND,
          prepared_png="assets/boot/badge/htn.png",
          cf="RGB565",
          trim_trailing_blank=True),
    Asset(name="boot",
          source="assets/boot/solana_logo_light.png",
          size=(240, 37),
          fit=True,
          symbol="boot_logo_solana",
          background=BOOT_BACKGROUND,
          prepared_png="assets/boot/badge/solana.png",
          cf="RGB565",
          trim_trailing_blank=True),
    # Teardown tour: board photos cropped to the 320x200 display frame (see
    # assets/teardown/*.png masters, 960x600). Opaque RGB565 keeps each side
    # at 128 KiB of flash; the app reuses the same array for the 2x zoom.
    Asset(name="teardown",
          source="assets/teardown",
          pattern="*.png",
          size=(320, 200),
          symbol="teardown_{stem}",
          cf="RGB565",
          trim_trailing_blank=True),
    # PAL masters are tightly alpha-cropped, inset by two pixels on each side,
    # and quantized to I8. Each badge asset is 13.5 KiB instead of 36.75 KiB
    # as RGB565A8, while preserving palette alpha around the illustration.
    Asset(name="pals",
          source="assets/pals/source",
          pattern="*.png",
          size=(112, 112),
          fit=True,
          autocrop_alpha=True,
          padding=2,
          symbol="pal_{stem}",
          prepared_png="assets/pals/badge/{stem}.png",
          cf="I8",
          quantize_colors=256,
          trim_trailing_blank=True),
    # Native-sized picker thumbnails avoid scaling the indexed full-size PALs
    # at runtime, and keep the nine-cell page's decode buffers small.
    Asset(name="pal-thumbnails",
          source="assets/pals/source",
          pattern="*.png",
          size=(38, 38),
          fit=True,
          autocrop_alpha=True,
          padding=1,
          symbol="pal_thumb_{stem}",
          cf="I8",
          quantize_colors=256,
          trim_trailing_blank=True),
)


def sources_for(asset: Asset) -> list:
    root = ROOT / asset.source
    if asset.pattern:
        if not root.is_dir():
            raise SystemExit(f"no source dir: {root}")
        return sorted(root.glob(asset.pattern))
    if not root.is_file():
        raise SystemExit(f"missing artwork: {root}")
    return [root]


def convert(png: Path, asset: Asset, out_dir: Path,
            prepared_root: Path) -> tuple:
    """Resize `png` to the asset's size and emit its LVGL C array."""
    symbol = asset.symbol.format(stem=png.stem)
    source = Image.open(png)
    image = source.convert("RGBA")
    if asset.require_transparent_png:
        if source.format != "PNG" or image.getchannel("A").getextrema()[0] != 0:
            raise SystemExit(f"{png}: icon must be a PNG with real transparent pixels")
    if asset.autocrop_alpha:
        bbox = image.getchannel("A").getbbox()
        if bbox:
            image = image.crop(bbox)
    if asset.size and image.size != asset.size:
        if asset.fit:
            inner_size = (asset.size[0] - 2 * asset.padding,
                          asset.size[1] - 2 * asset.padding)
            if inner_size[0] <= 0 or inner_size[1] <= 0:
                raise SystemExit(f"padding is too large for {asset.name}")
            image.thumbnail(inner_size, Image.Resampling.LANCZOS)
            canvas = Image.new("RGBA", asset.size)
            offset = ((asset.size[0] - image.width) // 2,
                      (asset.size[1] - image.height) // 2)
            canvas.alpha_composite(image, offset)
            image = canvas
        else:
            image = image.resize(asset.size, Image.Resampling.LANCZOS)
    if asset.background:
        background = Image.new("RGBA", image.size, (*asset.background, 0xff))
        image = Image.alpha_composite(background, image).convert("RGB")
    if asset.quantize_colors:
        image = image.quantize(
            colors=asset.quantize_colors,
            method=Image.Quantize.FASTOCTREE,
            dither=Image.Dither.FLOYDSTEINBERG)

    prepared = None
    if asset.prepared_png:
        prepared = prepared_root / asset.prepared_png.format(stem=png.stem)
        prepared.parent.mkdir(parents=True, exist_ok=True)
        image.save(prepared)

    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
        tmp_path = Path(tmp.name)
    try:
        image.save(tmp_path)
        subprocess.check_call([
            sys.executable, str(TOOL),
            "--ofmt", "C",
            "--cf", asset.cf,
            "--compress", "NONE",
            "--name", symbol,
            "-o", str(out_dir),
            str(tmp_path),
        ])
    finally:
        tmp_path.unlink(missing_ok=True)

    generated = out_dir / f"{symbol}.c"
    if asset.trim_trailing_blank or png.stem in asset.trim_trailing_blank_stems:
        generated.write_text(generated.read_text().rstrip() + "\n")
    return generated, prepared


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group_names = tuple(dict.fromkeys(a.name for a in ASSETS))
    parser.add_argument("groups", nargs="*", metavar="GROUP",
                        help=f"one or more of: {', '.join(group_names)}")
    parser.add_argument("--check", action="store_true",
                        help="regenerate into a temp dir and fail if the "
                             "checked-in output differs")
    args = parser.parse_args()

    selected = ASSETS
    if args.groups:
        unknown = [g for g in args.groups if g not in group_names]
        if unknown:
            parser.error(f"unknown group(s): {', '.join(unknown)}")
        selected = tuple(a for a in ASSETS if a.name in args.groups)

    with tempfile.TemporaryDirectory() as staging:
        staging_root = Path(staging)
        out_dir = staging_root / "generated" if args.check else OUT_DIR
        prepared_root = staging_root if args.check else ROOT
        out_dir.mkdir(parents=True, exist_ok=True)

        stale = []
        count = 0
        for asset in selected:
            for png in sources_for(asset):
                generated, prepared = convert(
                    png, asset, out_dir, prepared_root)
                count += 1
                if args.check:
                    committed = OUT_DIR / generated.name
                    if not committed.is_file() or not filecmp.cmp(
                            committed, generated, shallow=False):
                        stale.append(generated.name)
                    if prepared:
                        prepared_name = asset.prepared_png.format(stem=png.stem)
                        committed = ROOT / prepared_name
                        if not committed.is_file() or not filecmp.cmp(
                                committed, prepared, shallow=False):
                            stale.append(prepared_name)

        if args.check:
            if stale:
                print("stale generated assets: " + ", ".join(sorted(stale)),
                      file=sys.stderr)
                print("run tools/image-convert/regen-assets.py to update them",
                      file=sys.stderr)
                return 1
            print(f"checked {count} asset(s): all current")
            return 0

    print(f"regenerated {count} asset(s) into {OUT_DIR.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
