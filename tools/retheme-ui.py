#!/usr/bin/env python3
"""One-time migration from the legacy blue UI literals to BadgeTheme tokens.

Dry-run by default. Pass --write after reviewing the unified diff.
"""

from __future__ import annotations

import argparse
import difflib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

COLOR_REPLACEMENTS = {
    "0x0a0e1a": "BadgeTheme::BACKGROUND",
    "0x0f1422": "BadgeTheme::PANEL",
    "0x1a2332": "BadgeTheme::SURFACE",
    "0x1e2740": "BadgeTheme::TRACK",
    "0x2d4a6b": "BadgeTheme::BORDER",
    "0x00d9ff": "BadgeTheme::ACCENT_DETAIL",
    "0xe0e8f0": "BadgeTheme::TEXT",
    "0xa8b4c8": "BadgeTheme::TEXT_SOFT",
    "0x6a7a9a": "BadgeTheme::TEXT_MUTED",
    "0x6b7a8f": "BadgeTheme::TEXT_MUTED",
    "0x3a4560": "BadgeTheme::TEXT_DIM",
    "0x4a5568": "BadgeTheme::TEXT_DIM",
    "0xa0d8ff": "BadgeTheme::TEXT_SOFT",
}

# Only structural UI gets squared off. Circles and small game pieces remain
# round where their shape carries meaning.
SHARP_REPLACEMENTS = {
    "main/apps/app_launcher.cpp": {6: 0, 4: 0},
    "main/apps/app_buttons.cpp": {6: 0},
    "main/apps/app_test.cpp": {6: 0},
    "main/apps/app_eightball.cpp": {10: 0},
    "main/apps/app_blindbox.cpp": {10: 0},
    "main/apps/app_contacts.cpp": {4: 0, 3: 0},
    "main/apps/app_files.cpp": {4: 0, 3: 0},
    "main/apps/app_tictactoe.cpp": {6: 0},
    "main/apps/app_snake.cpp": {4: 0},
    "main/apps/app_tetris.cpp": {4: 0},
}


def migrate(path: Path, source: str) -> str:
    migrated = source
    for old, new in COLOR_REPLACEMENTS.items():
        migrated = migrated.replace(old, new)

    relative = path.relative_to(ROOT).as_posix()
    for old_radius, new_radius in SHARP_REPLACEMENTS.get(relative, {}).items():
        migrated = migrated.replace(
            f"lv_obj_set_style_radius(cell, {old_radius}, 0)",
            f"lv_obj_set_style_radius(cell, {new_radius}, 0)",
        )
        for name in (
            "grid_", "card", "row", "pad", "panel_", "list_panel_",
            "detail_panel_", "file_view_", "board_bg_", "bg",
        ):
            migrated = migrated.replace(
                f"lv_obj_set_style_radius({name}, {old_radius}, 0)",
                f"lv_obj_set_style_radius({name}, {new_radius}, 0)",
            )
    return migrated


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--write", action="store_true")
    args = parser.parse_args()

    changed = 0
    # Every screen the attendee sees. main/test.cpp is deliberately excluded:
    # it is the legacy JLC factory script and keeps its own standalone palette.
    paths = sorted((ROOT / "main" / "apps").glob("*.cpp"))
    paths.append(ROOT / "main" / "boot_splash.cpp")
    for path in paths:
        source = path.read_text()
        migrated = migrate(path, source)
        if migrated == source:
            continue
        changed += 1
        if args.write:
            path.write_text(migrated)
            continue
        print("".join(difflib.unified_diff(
            source.splitlines(keepends=True),
            migrated.splitlines(keepends=True),
            fromfile=str(path.relative_to(ROOT)),
            tofile=str(path.relative_to(ROOT)),
        )), end="")

    action = "updated" if args.write else "would update"
    print(f"\n{action} {changed} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
