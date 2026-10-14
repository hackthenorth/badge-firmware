#!/usr/bin/env python3
"""Assert the reboot-into-app contracts hold across the firmware.

OOM recovery, NFC/radio-enter reboot, and radio/NFC-exit reboot are spread over
several files that must agree: the App trait, the HAL session queries, the
registry rules, the boot routing, and the splash skip. Editing one side and
not the other fails silently as "the badge just switches directly" or "it
reboot-loops" — both need a badge in hand to notice.

    python3 tools/check-reboot-contracts.py   # exit 1 on drift
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
MAIN = REPO / "main"
APPS = MAIN / "apps"
HAL = MAIN / "hal"

FAILURES = []


def src(path):
    return (REPO / path).read_text()


def must_contain(path, needle, why):
    if needle not in src(path):
        FAILURES.append(f"{path}: missing {needle!r} ({why})")


def must_not_contain(path, needle, why):
    if needle in src(path):
        FAILURES.append(f"{path}: unexpected {needle!r} ({why})")


# 1. The trait: default false on App ...
must_contain("main/apps/app.h",
             "virtual bool uses_nfc() const { return false; }",
             "NFC traits remain available to the fresh-boot default")
must_contain("main/apps/app.h",
             "virtual bool needs_fresh_boot() const { return uses_nfc(); }",
             "fresh-boot entry includes NFC by default")

# ... true for exactly scanner, workshops, sponsors.
NFC_APPS = {"app_scanner.cpp", "app_sponsors.cpp", "app_workshops.cpp"}
overriders = set()
for p in sorted(APPS.glob("app_*.cpp")):
    if "uses_nfc() const override { return true; }" in p.read_text():
        overriders.add(p.name)
if overriders != NFC_APPS:
    FAILURES.append(
        f"uses_nfc() overriders = {sorted(overriders)}, "
        f"expected {sorted(NFC_APPS)} (enter-reboot is scanner/workshops/sponsors only; "
        "app->app payload handoffs stay direct)")

RADIO_APPS = {"app_share.cpp", "app_sync.cpp"}
fresh_overriders = {p.name for p in APPS.glob("app_*.cpp")
                   if "needs_fresh_boot() const override { return true; }" in p.read_text()}
if fresh_overriders != RADIO_APPS:
    FAILURES.append(f"fresh-boot radio apps = {sorted(fresh_overriders)}, "
                    f"expected {sorted(RADIO_APPS)}")

# 2. HAL session queries both exist (exit rule samples actual usage so Lua
# radio/NFC sessions reboot too, with no per-app trait needed).
must_contain("main/hal/hal_nfc.h", "bool hal_nfc_is_enabled();",
             "exit rule must see NFC sessions")
must_contain("main/hal/hal_nfc.cpp", "bool hal_nfc_is_enabled()",
             "exit rule must see NFC sessions")
must_contain("main/hal/hal_radio.h", "bool hal_radio_is_enabled();",
             "exit rule must see radio sessions")
must_contain("main/hal/hal_radio.cpp", "bool hal_radio_is_enabled()",
             "exit rule must see radio sessions")

# 3. Registry: both rules, the bypass, and the session sampling.
for needle, why in [
    ("app_reboot_into_app(a)", "reboot-enter rule"),
    ("app_reboot_to_launcher(slug)", "reboot-exit rule"),
    ("hal_nfc_is_enabled()", "exit rule samples NFC"),
    ("hal_radio_is_enabled()", "exit rule samples radio"),
    ("s_reboot_bypass", "post-reboot boot must not restart out of its target"),
    ("a->needs_fresh_boot()", "enter rule includes NFC and memory-heavy radio apps"),
]:
    must_contain("main/apps/app_registry.cpp", needle, why)
must_contain("main/apps/app_registry.h", "bool reboot_bypass",
             "boot path must bypass the enter rule once")

# 4. Reboot module: brownout hardening.
# - honored only after our own SW restart ...
must_contain("main/apps/app_reboot.cpp", "ESP_RST_SW",
             "only our own esp_restart() may redirect the boot")
# - ... consumed (deleted) at boot so one request fires exactly once ...
if src("main/apps/app_reboot.cpp").count("unlink(kIntentPath)") < 2:
    FAILURES.append("main/apps/app_reboot.cpp: intent must be unlinked both "
                    "on non-SW resets and when consumed (once-only limiter)")
# - ... persisted without malloc (the OOM path may have no heap left) ...
must_not_contain("main/apps/app_reboot.cpp", "malloc(",
                 "OOM request path must not allocate")
must_contain("main/apps/app_reboot.cpp", "open(kIntentPath",
             "intent must use raw POSIX I/O, not hal_config/NVS")
# - ... and every request path restarts.
for fn in ["app_reboot_into_slug", "app_reboot_to_launcher"]:
    body = src("main/apps/app_reboot.cpp")
    m = re.search(rf"{fn}\b.*?\n\}}", body, re.S)
    if not m or "esp_restart()" not in m.group(0):
        FAILURES.append(f"main/apps/app_reboot.cpp: {fn} must esp_restart()")

# 5. Boot routing: consume before the first frame, bypass start into target,
# grid start on focus, and the splash card never painted on reboot boots
# (panel stays dark until the destination is revealed directly).
for needle, why in [
    ("app_reboot_consume_pending", "boot must consume the intent"),
    ("app_registry_start(app_launcher_instance(), reboot_target, true)",
     "boot into target must bypass the enter rule"),
    ("skip_splash_boot", "splash decision must precede the first frame"),
    ("if (!FACTORY_TEST_BUILD && !skip_splash_boot) boot_splash_show();",
     "reboot boots must never paint the splash card"),
    ("if (skip_splash) hal_display_show();",
     "reboot boots reveal the destination directly"),
    ("app_launcher_instance(), app_launcher_instance()",
     "reboot-exit must land on the grid, not the badge ID page"),
]:
    must_contain("main/main.cpp", needle, why)
must_not_contain("main/boot_splash.h", "boot_splash_skip",
                 "no card is ever painted on reboot boots, so no skip entry point")
must_not_contain("main/main.cpp", "boot_splash_skip",
                 "no card is ever painted on reboot boots, so no skip entry point")

# The consume must precede the first frame: FS/config/intent before the
# splash show call in main.cpp.
_body = src("main/main.cpp")
if _body.find("hal_fs_init();") > _body.find("boot_splash_show();"):
    FAILURES.append("main/main.cpp: hal_fs_init()/consume must run before "
                    "boot_splash_show() so reboot boots skip the card")

# 6. Launcher restores the stashed cursor one-shot.
must_contain("main/apps/app_launcher.cpp", "app_reboot_take_launcher_focus",
             "seamless exit must refocus the exited app")

# 7. At least one OOM detector is wired (outrun framebuffer).
must_contain("main/apps/app_outrun.cpp", "app_reboot_into_app(this)",
             "outrun malloc failure must reboot back, not strand on a card")

if FAILURES:
    print("reboot contract drift:")
    for f in FAILURES:
        print(f"  - {f}")
    sys.exit(1)
print(f"reboot contracts ok ({len(NFC_APPS)} NFC + {len(RADIO_APPS)} radio apps, enter+exit rules wired)")
