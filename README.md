# badge-firmware

Firmware for the 2026 Hacker Badge Project.

This repository contains the badge’s main firmware: a launcher-driven interface, built-in utilities and games, hardware-integrated apps, unlock progression, and support for user-installed Lua applications stored on LittleFS.

## Overview

The firmware boots into a grid-based launcher that presents apps across multiple pages. From there, the badge can run a mix of pre-installed tools, motion and light demos, NFC-driven experiences, mini-games, and user-authored Lua apps.

At a high level, the firmware supports:

- A consistent app lifecycle for launch, exit, input, and periodic updates.
- A launcher UI with paging, focus navigation, and a back-to-home behavior.
- Unlockable apps that can be hidden until discovered or earned.
- On-demand access to higher-power hardware blocks such as NFC and radio.
- User-installed Lua apps loaded dynamically from the filesystem.

## Feature Areas

### Launcher and UI

- 4x4 grid launcher with page navigation.
- Embedded icon support, text icons, and optional on-disk app artwork.
- Centralized button dispatch with a consistent return-to-launcher shortcut.

The default order is My Badge (QR), Connect, Scanner, Share, Solana Scavenger
Hunt, Sponsors, Workshop Quest, Mood LEDs, Socials, Schedule, Museum, then
Info & Rules.

Fresh badges, factory resets, and explicit provisioning mode resets use this order. Existing
badges that still have the previous default order receive a one-time update;
custom arrangements and the order of subsequently unlocked apps are preserved.

### Utilities and System Apps

- Diagnostics and memory information.
- Power-related status and monitoring.
- File browsing and filesystem tools.
- Contact storage and badge-oriented data exchange.
- Level, dice, and eight-ball style tools.
- Flashlight in diagnostic mode.

### Hardware and Interaction

- Button testing and input feedback.
- Accelerometer-aware apps and motion interactions.
- NFC-enabled features and scan-based unlock flow.
- Light control and synchronized light shows.
- Badge-specific interaction apps built around physical behavior.

### Games

- Snake.
- Tetris.
- Breakout.
- Tic-tac-toe.
- Simon.
- Maze.
- Flappy-style gameplay.
- Badge bump and other social or challenge-style mini-games.

### Solana Scavenger Hunt

Physical stations at the event. Open the **Solana Scavenger Hunt** app, tap the
station's NFC tag, and the badge reveals what was inside.

The badge has no WiFi and no HTTP stack, so this runs over three pieces:

| Piece | Where | What it does |
|---|---|---|
| Badge app | `main/apps/app_blindbox.cpp` | Reads the tag, builds the request JSON, broadcasts it over BLE, parses the reply, shows the item. |
| Relay | `tools/relay_firmware/` | A mains-powered ESP32-C3 that forwards BLE frames to an HTTP webhook and back. Never parses the payload. |
| Server | `tools/relay_server/` | Express app on Vercel that answers with a random fruit. Stand-in for the real thing. |

The wire format is `main/apps/bbx_proto.h` — connectionless BLE 5 extended
advertising sharing the same `0xFFFF` manufacturer-data channel as badge
bumping, discriminated by a `BBX1` magic. One frame each way, 232 bytes of
body, no fragmentation.

There are three ways to trigger an open, cycled with **Start**:

- **WEB** — the current proof-of-concept default. Connect
  `tools/blindbox-web/index.html` with Web Serial and press **A**. The page adds
  its configured box slug, POSTs the badge identity, and returns a
  `{message,url}` response for the badge to render as text plus a QR code.

- **TAP** — hold the station's NFC tag to the back of the badge. Tags hold NDEF
  text of the form `box:<id>`, e.g. `box:orchid-01`. The radio stays powered
  down until a tag is read.
- **NEARBY** — no NFC at all. The badge listens for the relay's beacons, which
  carry the station's box id, and fires when the smoothed signal strength says
  you're close enough. The idle screen shows the nearest box and its live dBm;
  **Up/Down** tunes the trigger range on the spot. Costs battery — the radio
  scans continuously in this mode.

The app is included by default, fifth in the launcher after Share.
Its existing `blindbox` slug is retained for stored configuration and tooling
compatibility. The launcher icon uses the Solana mark from
`assets/boot/solana_logo_light.png`, prepared as the 96×96
`assets/icons/blindbox.png` master for the standard icon generator.

See `tools/relay_firmware/README.md` for setup and bring-up order.

### Extensibility

- Lua app loading from `/littlefs/apps`.
- Per-app manifests for display name, slug, and icon.
- Optional image-based icons for scripts.
- Script lifecycle hooks for entering, ticking, handling buttons, and exiting.
- A restricted Lua runtime with scoped storage, files, and local module loading.

## Project Layout

- `main/` - application entry point, app registry, launcher, hardware abstractions, and built-in apps.
- `main/apps/` - launcher, utilities, games, unlock logic, Lua bindings, and app implementations.
- `managed_components/` - third-party and shared component dependencies.
- `assets/` - static assets such as icons.
- `tools/` - helper utilities for badge development.
- `tools/relay_firmware/` - Solana Scavenger Hunt BLE-to-HTTP relay. A separate ESP-IDF project; build it from inside that directory, not from the repo root.
- `tools/relay_server/` - Solana Scavenger Hunt demo webhook (Express on Vercel).

## Build Requirements

- ESP-IDF with the dependencies declared in `main/idf_component.yml`.
- The firmware is built as an ESP-IDF project.
- The repository is set up for a minimal build configuration in the top-level `CMakeLists.txt`.

## Build

With the project's ESP-IDF v5.5.3 installation:

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
python "$IDF_PATH/tools/idf.py" build
```

If you are using the VS Code ESP-IDF extension, you can also build from the command palette or the extension’s build actions.

## Flash and Monitor

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
python "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodemYOUR_PORT flash
python "$IDF_PATH/tools/idf.py" -p /dev/cu.usbmodemYOUR_PORT monitor
```

Replace the port with your badge's port. If native USB will not connect, hold
Start while plugging in USB and retry; reboot normally afterward. Close other
serial tools before connecting. See the [browser flashing guide](tools/web-flasher/README.md)
for factory versus launcher firmware and the Lua upload workflow.

## User Lua Apps

Use the [browser IDE](https://badge.hackthenorth.com/ide/) to install Lua apps
without building or reflashing firmware. The [complete app guide](tools/badgectl/web/README.md)
contains connection/upload steps, the supported APIs and limits, a prompt for
web agents, and seven full app examples with config and code in one file.

Give that Markdown file to an agent, describe your app, and paste its complete
Lua response into **Import app**. Then **Connect → Push** installs it. The IDE
splits the manifest header and code into `/littlefs/apps/<slug>/manifest.cfg`
and `main.lua`. Manual multi-file apps and image icons are also supported.

The [Lua API reference](docs/LUA-API.md) covers the runtime bindings.
For a system firmware update, use the [firmware flashing guide](tools/web-flasher/README.md);
the Lua IDE needs the badge running normally, while firmware recovery may need
Start held during USB connection to enter download mode.

## Notes

- NFC and radio are enabled only when an app needs them.
- The registry disables those subsystems again when switching apps.
- Some built-in apps may remain locked until unlocked through badge progression.
- The codebase is organized around a shared app interface so new features can fit into the launcher cleanly.

## Release Builds

The repository includes GitHub Actions support for building the firmware and generating a size report on pull requests. Tagged releases also package the build output as a release asset.

## License

This project is licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).
