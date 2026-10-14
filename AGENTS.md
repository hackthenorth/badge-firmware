# AGENTS.md

2026 Hacker Badge firmware + host tooling. ESP-IDF (ESP32) project rooted at
this directory: LVGL launcher UI, built-in C++ apps, user Lua apps on LittleFS,
USB-Serial-JTAG console. See `README.md` and `CLAUDE.md`.

## What this is

- Boots into a 4x4 paged grid launcher (`main/apps/app_launcher.cpp`,
  `app_registry.cpp`, `launcher_slugs.cpp`) with focus nav, return-to-home (HOME),
  unlockable apps, and on-demand NFC/radio power.
- Built-in apps live in `main/apps/` (games, tools, NFC/social, diagnostics);
  `main/main.cpp` registration order is launcher order (badge ID first).
- User Lua apps load from `/littlefs/apps/<name>/{manifest.cfg, main.lua}` with
  `on_enter/on_tick/on_button/on_exit` hooks; examples in `assets/lua_apps/`.
- `partitions.csv`: `factory` app slot `0x2A0000`, `storage` LittleFS `0x140000`.
  Identify which overflows before resizing.
- `main/CMakeLists.txt` globs `SRC_DIRS "." "hal" "apps" "apps/icons_generated"`.

## Firmware architecture

- Entry `app_main` in [main.cpp](main/main.cpp): `hal_display_init()` +
  `boot_splash_show()` first, then `hal_fs/identity/config`,
  `launcher_slugs_load`, `hal_i2c/buttons/accel/lights/sleep_init`.
- NFC and radio are enable-on-demand: registry force-disables both on every app
  switch; the entering app re-enables in `on_enter()`.
- `FACTORY_TEST_BUILD` (`BADGE_FACTORY_TEST=1`) boots to `factory_test_run()`
  instead of the launcher.
- App framework ([app.h](main/apps/app.h)): subclass `App` with
  `name()/icon()/slug()/on_enter(root)` plus optional
  `on_exit/on_tick/on_button`. All hooks run under the LVGL lock; a spawned
  FreeRTOS task must take `lvgl_port_lock(0)` itself before touching LVGL.
- `access()` is the only visibility control: `Default` (always-visible boot
  bundle, can't be moved/dropped), `Public` (hidden until its slug is in the
  NFC unlock set), `Internal` (staff/diagnostic, never unlocked by public tags).
- Minimal template: [app_dice.cpp](main/apps/app_dice.cpp).

## Tools (host-side)

`tools/` holds: `badge/`, `badge-provision/`, `badgectl/`, `check-bbx-proto.py`,
`chess-perft/`, `emulator/`, `image-convert/`, `relay_firmware/`,
`relay_server/`, `retheme-ui.py`, `serial-buttons/`, `sync_station/`,
`web-flasher/`.

### Browser emulator (`tools/emulator/`)

The firmware compiled to WebAssembly in one self-contained `index.html`: no
badge or serial port needed, and public-facing (a quick play for website
visitors). Buttons on a badge photo, a Shake button for the accelerometer, NFC
tag taps from the firmware's own catalogs, Lua app install, LEDs and the USB
console; everything else sits under Advanced. See
[README](tools/emulator/README.md).

- `tools/emulator/build.py` regenerates `index.html` (`--install-emsdk` once to
  fetch Emscripten into the gitignored `.emsdk/`). Rebuild after firmware edits;
  it is incremental and takes seconds, unlike `idf.py build`.
- Everything under `main/` is compiled unmodified except the hardware HAL files
  listed in `REPLACED_HAL`; their stand-ins are `src/emu_hal.cpp`. A new ESP-IDF
  include needs a matching header under `src/idf/`.
- `window.badge` (`press`, `shake`, `tap`, `exec`) scripts the page; `exec`
  returns console output, so `badge.exec('uitree')` reads the screen as text.
- Heap limits, driver timing and tilt are not emulated: confirm memory-sensitive
  or radio/NFC driver changes on hardware with `tools/badge/badge.py`.

### Interactive badge dev (`tools/badge/badge.py`)

Primary vibecoding loop: press buttons and read the screen back over the USB
console, no human watching the badge. Stdlib only, no venv. Firmware side is
`shot` (`main/hal/hal_screencap.cpp`) and `uitree` (`main/hal/hal_uitree.cpp`).

```sh
tools/badge/badge.py shot screen.png
tools/badge/badge.py press Right press A settle 500 shot /tmp/after.png
tools/badge/badge.py tree
tools/badge/badge.py --out-dir /tmp run tools/badge/examples/launcher-tour.txt
```

- Verbs chain left to right over one open port: `press`, `settle`, `shot`,
  `tree`, `console`, `expect`, `run`. See [README](tools/badge/README.md).
- Prefer `tree` over `shot` for "what does the label say / is it focused" —
  ~1000x cheaper than a PNG and it diffs. `settle` after a press (button events
  drain on a 50 ms LVGL timer; `on_enter()` may animate).
- `expect` turns a macro into a check (exit 1 on mismatch); `run` splices a
  script file (`examples/launcher-tour.txt`, `chess-pair.txt`).
- `duet.py` drives two badges (`a:`/`b:`/`both:` prefixes) for pairing/radio
  checks that one badge cannot observe alone.
- Protocol traps: send a bare `\r` (console RX is `ESP_LINE_ENDINGS_CR`), chunk
  writes under 256 bytes (USB-Serial-JTAG RX ring). Reading the screen freezes
  the UI for a few hundred ms. Only one process may own the port (fights with
  `idf.py monitor`, `serial-buttons`).

### Other tools

- `badgectl/` (`web/` + `sample/`): web UI entry.
- `image-convert/regen-assets.py`: one script + `ASSETS` manifest generating
  `main/apps/icons_generated/` LVGL C arrays. New artwork is a manifest entry,
  not a new script; `--check` verifies checked-in output is current. Source PNGs
  are 96x96, downscaled to 48x48.
- `serial-buttons/`: older webcam-based button HAL predecessor to `badge.py`.
- Blind-box chain: `main/apps/app_blindbox.cpp` + `bbx_proto.h` (badge),
  `relay_firmware/` (ESP32-C3 BLE-to-HTTP forwarder), `relay_server/`
  (Express/Vercel stand-in). `tools/check-bbx-proto.py` keeps the `bbx_proto.h`
  copies in agreement (CI runs it).
- `badge-provision/`: provisioning docs including the CR/256-byte traps.

## Commands

- `idf.py build` — build the project (multi-minute; ESP-IDF env must be active
  via the `idf` skill / `~/.espressif/tools/activate_idf_v5.5.3.sh`).
- `idf.py reconfigure` is cheap and required after adding a new `.cpp` (globbed
  at configure time). Mention it before running.
- `idf.py flash` / `idf.py monitor` touch the device — never run without
  explicit permission. Don't auto-build after edits; the user triggers builds.

## Code Map

- `main` — application source (`hal/`, `apps/`, entry `main.cpp`, `pins.h`).
- `docs` — project documentation.
- `.github` — project configuration (ESP-IDF build + size-report workflow).
- `tools` — host tooling above; `assets/lua_apps/` holds Lua examples.

## Conventions

- Use quoted `#include` for local headers and angle-bracket `#include` for
  system headers.
- Write firmware apps as `.cpp` files named `main/apps/app_<name>.cpp` with an
  `app_<name>_instance()` factory; forward-declare it in `app_launcher.h` and
  register in `main.cpp`.
- Front-load includes that have bitten before: `unistd.h` for
  `unlink/access/usleep`, `sys/stat.h` for `mkdir/stat`,
  `freertos/FreeRTOS.h` before task/queue headers; direct flash access needs
  `esp_flash` in `REQUIRES`; console is USB-Serial-JTAG, never assume UART0;
  `printf` truncation warnings are errors (`-Werror=format-truncation`).
- I2C is one shared bus (accel `0x19`, NFC `0x26`); LCD uses DMA stripe buffers,
  not full framebuffers.

## Flashing (verified 2026-09-03)

- The activation script does NOT put `idf.py` on PATH. Invoke it as
  `python "$IDF_PATH/tools/idf.py" <cmd>` after sourcing
  `~/.espressif/tools/activate_idf_v5.5.3.sh` in the same shell.
- `idf.py flash` may fail to connect: native USB-Serial-JTAG has no
  auto-reset. The proven form is explicit esptool from `build/`:
  `<venv>/bin/python $IDF_PATH/tools/.../esptool/esptool.py -p
  /dev/tty.usbmodem1101 -b 460800 --before default_reset --after hard_reset
  --chip esp32c3 write_flash --flash_mode dio --flash_freq 80m
  --flash_size 4MB 0x0 bootloader/bootloader.bin 0x10000 hello_world.bin
  0x8000 partition_table/partition-table.bin`
- If esptool reports "No serial data received", put the badge in download
  mode first: hold Start (GPIO9 strapping pin) while plugging in USB.
  Blank screen is expected — that is download mode, not a brick.
- `idf.py reconfigure` (or any CMakeLists edit) is required before the
  first build after adding a `.cpp` file: sources are globbed at
  configure time.
