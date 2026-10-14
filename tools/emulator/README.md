# Badge emulator

`index.html` is the badge in a browser: the firmware under `main/` compiled to
WebAssembly, with the screen drawn onto a scan of the board
(`high-res-crop.jpg`). It is one self-contained file: host it anywhere, or open
it directly (`file://` works) in a current Chrome, Firefox or Safari. No
server, no badge, no USB.

It is the real firmware, not a re-implementation. Every source under `main/`
is compiled unmodified except the HAL files that talk to hardware (display,
buttons, accelerometer, NFC, radio, filesystem, I2C), so the launcher, all
built-in apps, Lua apps, onboarding, the sleep card, the reboot-into-app flow
and the USB console commands behave as they do on a badge.

## Using it

The page is set up for a quick play. A fresh badge belongs to "Ada Lovelace",
has every public app unlocked, and starts in the firmware's tutorial; **Skip
tutorial** under the badge goes straight to the app grid.

| Badge | Emulator |
|---|---|
| Buttons | Click the buttons on the photo, or the keyboard: arrows, `A`, `B`, `Enter` (Start), `Space` (Home). Holding works. |
| Accelerometer | **Shake the badge** (or `S`). Hold to keep shaking. The badge otherwise rests face-up. |
| NFC | Pick an app, sponsor or workshop under **NFC tags** and **Tap**. The lists come from the firmware's own catalogs. |
| Lua apps | Install a bundled example or a folder of your own under **Lua apps**. |
| USB console | The **Console** panel. `help` lists the firmware's commands. |
| LEDs | Drawn on the photo at their real positions. |
| Switches, USB port | Clickable on the photo: power, the side switch, and jump to the console. |

A tag only reads while an app has the reader on (Scanner, Sponsors, Workshop
Quest). The panel says so when a tap goes nowhere. With every app unlocked, an
app tag reads as "Already unlocked"; pick **Starter set** under Advanced to see
unlocks happen.

Everything else is under **Advanced**: power, reset, sleep and screenshot, the
badge's owner and role, which apps are shown, faulty and custom NFC tags,
Bluetooth (every open emulator window is a badge in range of the others), and
exporting, importing or erasing the flash, which is kept in the browser's
`localStorage`.

`?badge=2` in the address is a second, independent badge with its own identity,
storage and Bluetooth address.

### Scripting

The page exposes `window.badge` for the devtools console or a browser driver:

```js
await badge.press('Right');          // press and release
badge.hold('A'); badge.release('A');
badge.shake(600);
badge.tap('work:openai-api');        // NFC text record
badge.exec('uitree');                // console command; returns its output lines
badge.skipTutorial();
```

## Building

```sh
tools/emulator/build.py --install-emsdk   # first time: fetches Emscripten (~1.8 GB)
tools/emulator/build.py                   # afterwards: incremental, a few seconds
```

Needs Python 3 with Pillow, the managed components (`idf.py reconfigure`
fetches them) and ESP-IDF's cJSON (`IDF_PATH`, or the standard
`~/.espressif/v5.5.3` install). `emcc` on `PATH` or `$EMSDK` is used in
preference to the local `.emsdk/`.

Rebuild after changing firmware sources; `index.html` is generated, so edit
`src/shell.html` rather than the output. Objects are cached in `.build/`.

## How it is put together

- `src/emu_hal.cpp` replaces `hal_display`, `hal_buttons`, `hal_accel`,
  `hal_nfc`, `hal_radio`, `hal_fs` and `hal_i2c`, and sits underneath the real
  `hal_lights.cpp` and `hal_sleep.cpp` as the LED strip and sleep drivers.
- `src/idf/` is the slice of ESP-IDF and FreeRTOS headers the firmware
  includes; `src/emu_idf.cpp` implements it (timer, NVS, restart, partitions).
- `src/emu_console.cpp` is `esp_console` without the REPL loop, so
  `hal_console.cpp` registers its commands unchanged.
- `src/lv_kconfig.h` mirrors the firmware's LVGL Kconfig: RGB565, the same
  fonts, the same 64 KB object pool. The build warns if it drifts from
  `build/config/sdkconfig.h`.
- `src/shell.html` is the page. One WebAssembly instance is one boot;
  `esp_restart()` discards it and starts another with `ESP_RST_SW`, which is
  what makes the firmware's reboot-into-app and reboot-to-launcher flows work.
- Time is virtual and advances with the wall clock, so a 20 ms app tick stays
  50 Hz regardless of the display's frame rate.
- The build straightens `high-res-crop.jpg`, cuts the board out of the scanner
  background and embeds it. The screen, button and LED positions in
  `src/shell.html` (`GEOM`) are pixels of that straightened scan; replacing the
  photo means re-measuring them and `PHOTO_ROTATION` in `build.py`.
- The page's defaults are constants at the top of the script in
  `src/shell.html`: `DEFAULT_APP_MODE` (which apps start unlocked) and `OWNERS`
  (the name on a fresh badge).

## What is not emulated

- **Memory limits.** A badge has about 182 KB of heap; the emulator does not
  cap it, so out-of-memory paths never fire. LVGL's 64 KB pool is real.
- **Speed.** Code runs as fast as the browser allows, not at 80 MHz.
- **The drivers themselves.** Bugs in the replaced HAL files (I2C contention,
  SPI timing, NimBLE) cannot show up here.
- **Tilt.** Only rest and shake. Tilt games open but have nothing to steer by.
- **Anything off-badge.** Sync stations, the scavenger-hunt relay and real
  phones are not there, so those apps wait for a peer that never answers.
- **Raw serial payloads.** `put`, `badge_token` and `badge_profile` expect bytes
  after the command line; the console panel sends lines only. Use the Lua apps
  panel to install apps instead.
- **The factory-test build** (`BADGE_FACTORY_TEST=1`).

Use a real badge and `tools/badge/badge.py` for anything on that list.
