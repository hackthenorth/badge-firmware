# rapid-flash

Hotplug-triggered ESP32-C3 firmware flasher. Plug a badge in, it gets flashed
— automatically, within a fraction of a second, with no clicks. Unplug it,
plug the next one. Boards flash concurrently, so plugging a second badge
while the first is still writing just works.

Single static-ish binary (Rust + `espflash` linked in, no Python, no venv,
no external `esptool`), builds for macOS / Windows / Linux.

## Quick start

With no arguments the tool flashes the firmware image set baked into the
binary at compile time (from `firmware/`), so a bare binary with nothing
else around it just works:

```sh
rapid-flash
```

On startup it prints the image identity — a SHA-256 over the bytes at
their flash offsets plus a GitHub-style identicon mosaic derived from
that hash. Same bytes on any machine give the same hash and the same
mosaic, so compare them across stations before a production run:

```
rapid-flash: 3 part(s), 2686928 bytes total @ 460800 baud (embedded firmware)
  0x00000: <embedded>/bootloader/bootloader.bin (21024 bytes)
  ...
firmware sha256: 4eab89e5a217f2ce9b148753df3fcaecb41fd6da82e0efedbd4a5c1fd855a11d
```

(`NO_COLOR=1` renders the mosaic as text instead of color blocks.)

To flash a different build, pass it explicitly — all current options
still work and override the embedded set. From an ESP-IDF build
directory, `flasher_args.json` already describes the full image set
(bootloader + partition table + app):

```sh
# From the repo root, after `idf.py build`:
cargo run --release --manifest-path tools/rapid-flash/Cargo.toml -- \
  --flasher-args build/flasher_args.json
```

Plug a badge in. You should see:

```
[E8:3D:C1:27:C4:88] detected on /dev/cu.usbmodem1101, flashing...
[OK] E8:3D:C1:27:C4:88 (/dev/cu.usbmodem1101): flashed 2628784 bytes in 12.8s
```

Or hand-pick parts:

```sh
rapid-flash \
  --part 0x0:build/bootloader/bootloader.bin \
  --part 0x8000:build/partition_table/partition-table.bin \
  --part 0x10000:build/hello_world.bin \
  --log-csv /tmp/flash-log.csv
```

Three firmware sets are baked in — badge (default), relay, sync station —
so one binary flashes any of the three boards:

```sh
rapid-flash          # badge firmware (default)
rapid-flash --relay  # tools/relay_firmware image set
rapid-flash --sync   # tools/sync_station image set
```

`--relay` / `--sync` conflict with each other and with `--part` /
`--flasher-args`. The startup line names the set, and the hash/mosaic cover
whichever set is selected — compare them across stations per set before a
production run.

To refresh a baked-in set, rebuild that firmware (`idf.py build` in its
directory), copy its `flasher_args.json` + bins over the matching
`firmware/<set>/` files (`badge`, `relay`, or `sync`), and rebuild
rapid-flash. The `dist/` binaries only carry the new sets after the next
tagged release build.

Other useful flags:

Every board is verified after writing — unconditionally, no flag to skip
it (production policy). A `[FAIL]` + nonzero exit means the board is not
shipped.

| Flag | Effect |
|---|---|
| `--relay` | Flash the baked-in relay firmware instead of the badge default |
| `--sync` | Flash the baked-in sync-station firmware instead of the badge default |
| `--once` | Flash boards already present, then exit (no monitoring) |
| `--dry-run` | Detect and report only; never opens a port |
| `--no-initial-scan` | Ignore boards already plugged in; flash arrivals only |
| `--vid-pid 10c4:ea60` | Extra USB VID:PID to accept (repeatable; default `303a:1001`) |
| `--allow-any-usb` | Probe every USB serial port (chip gate still applies) |
| `--allow-any-chip` | DANGER: flash any ESP chip, not just C3 |
| `--no-verify` | Skip post-write verify (faster, less safe) |
| `--baud 921600` | Flash baud rate (connect always starts at 115200) |
| `--retries 2` | Extra attempts per plug-in after a failure |
| `--log-csv PATH` | Append one row per attempt (timestamp, device, result, …) |

## Safety model

Three layers, in order:

1. **USB VID:PID prefilter** — default `303a:1001` (Espressif native
   USB-Serial-JTAG, what the badge exposes). Non-USB ports (Bluetooth, PCI)
   are never touched.
2. **Mandatory ESP32-C3 chip gate** — after connecting, the detected chip
   must be ESP32-C3 or the run aborts as `REFUSED` *before any erase or
   write*. This is enforced by `espflash` itself (`ChipMismatch`), plus a
   belt-and-suspenders check in the tool. A laptop, phone, or wrong dev
   board on the same hub cannot be flashed.
3. **Exactly-once per plug-in** — a board is identified by its USB serial
   (which on ESP parts *is* the MAC, e.g. `E8:3D:C1:27:C4:88`), so the
   macOS `/dev/cu.*` + `/dev/tty.*` double-enumeration flashes once, and the
   post-flash reboot never triggers a reflash loop. Unplug + replug re-arms.

Ctrl-C stops watching but waits for in-flight flashes to land — killing the
process mid-write is how boards get bricked. (Ctrl-C twice abandons.)

## Concurrency

One OS thread per board, no global serial lock. Each worker owns its port
handle end-to-end: settle delay → open → connect → chip gate → write all
parts → verify → hard reset. Presence tracking (arrived / in-flight /
done / vanished) is the only shared state, under one mutex.

## Production runs (hundreds/thousands of boards)

Measured per-board time for the ~2.6 MB image set (ESP32-C3, stub loader,
460800 baud, verify always on): **~12–13 s**. The baud rate is not the
bottleneck (921600 saves ~1 s); stub connect, flash erase+write, and the
verify readback dominate. Throughput comes from parallelism, not tuning:

- **Concurrent slots:** one operator with a powered hub flashes 4 boards
  at once (one thread per board, no shared state). ~5 boards/min/slot →
  ~20/min → 2500 boards in ~2 hours on one station.
- **Multiple stations:** the binary + `firmware/` dir are fully portable;
  4 stations ≈ 30 min for 2500. Use `--log-csv` on each — the USB serial
  (== MAC) in every row gives per-unit traceability.

Practical notes: powered USB hubs (bus power sags cause brownout
failures mid-write), short cables, and keep 921600 for short/clean setups
only — a failed board costs more than the 1 s it saves.

## Distributing binaries

Prebuilt files live in `dist/` (both macOS archs, Windows x64, Linux x64 —
each ~2.5 MB, no installer, no runtime). Reproduce them with:

```sh
cd tools/rapid-flash
cargo build --release                                    # native macOS (arm64)
cargo build --release --target x86_64-apple-darwin       # Intel Macs
CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER=x86_64-w64-mingw32-gcc \
  cargo build --release --target x86_64-pc-windows-gnu   # Windows (needs mingw-w64)
# Linux x86_64: build inside Docker (needs a real Linux linker + libudev):
docker run --rm --platform linux/amd64 -v "$PWD/../..":/work -w /work rust:1-bookworm \
  bash -c "apt-get update -qq && apt-get install -y -qq libudev-dev pkg-config && \
  cargo build --release --target x86_64-unknown-linux-gnu \
  --manifest-path tools/rapid-flash/Cargo.toml"
```

Hand out one binary: the `firmware/` image set is baked in, so it
flashes standalone with no arguments and no sidecar files (this roughly
doubles the binary to ~5 MB). To refresh the baked-in firmware, copy the
new `flasher_args.json` + bins into `firmware/` and rebuild — the
startup hash/mosaic changes with the bytes, which is how stations
confirm they agree. No admin needed except serial-port access:

- **Linux:** user must be in `dialout` (or install a udev rule for
  `303a:1001`), else ports open-fail with "permission denied".
- **macOS / Windows:** no driver needed for native USB-JTAG boards.

CI (`.github/workflows/rapid-flash.yml`) builds release binaries for
macOS, Windows, and Linux on every `rapid-flash-v*` tag and attaches them
as artifacts; PRs touching `tools/rapid-flash/` run `cargo test` + a
release check build instead.

## Developing

```sh
cd tools/rapid-flash
cargo test     # unit tests: parsing, filter, identity, flasher_args loader
cargo build    # debug build
```

Live test against a plugged badge without flashing: `--dry-run --once`.
