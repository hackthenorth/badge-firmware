# badge.py — drive the badge without touching it

Presses buttons and reads the screen back over the USB console, so a UI change
can be checked without a human watching the badge (or a webcam pointed at it,
which is what `tools/serial-buttons` does).

```sh
tools/badge/badge.py shot screen.png
tools/badge/badge.py press Right press A settle 500 shot after.png
tools/badge/badge.py tree
tools/badge/badge.py --out-dir /tmp run tools/badge/examples/launcher-tour.txt
```

No dependencies — stdlib only, no venv, no `pip install`. Needs the firmware
from this branch: `shot` and `uitree` are new console commands.

## Verbs

Verbs run left to right in one invocation, over a single open port. Opening the
port per action is slow and races the console, so chain instead of looping.

| Verb | Does |
|---|---|
| `press <Name>` | `A B Home Down Left Right Up Aux1 Start`, case-insensitive |
| `settle <ms>` | Wait. Button events drain on a 50 ms LVGL timer, and `on_enter()` may animate |
| `shot <out.png>` | Capture the screen |
| `tree [out.txt]` | Dump the LVGL widget tree; prints to stdout with no path |
| `console <cmd>` | Run any console command and print its output |
| `console radio` | Read the retained radio startup error, controller state, and heap; does not activate RF |
| `console "radio probe"` | Initialize/check the BLE controller and host without starting advertisements, scanning, or sending app data |
| `expect <text>` | Assert the last `tree`/`console` output contained `text`; exit 1 if not |
| `run <script>` | Splice in a script file's verbs |

Options: `--port` (else `$BADGE_PORT`, else autodetect), `--out-dir`,
`--timeout`, `--invert`, `--debug`.

## Scripts

One verb per line, `#` comments, shell-style quoting. `run` expands at parse
time, so a script and a command line are the same program by the time anything
talks to the badge. See `examples/launcher-tour.txt`.

`expect` is what makes a script a check rather than a macro — it fails the run
with a non-zero exit when the UI didn't end up where it should.

## Why `tree` and not just screenshots

`shot` gives you pixels; `uitree` gives you what the UI *is* — class, absolute
coords, state flags, and label text:

```
UITREE
screen 320x240
  lv_obj 0,0 320x240
    [0] lv_label 12,8 96x20 text="Launcher"
    [1] lv_button 12,40 64x64 state=FOCUSED
END nodes=37 depth=4 truncated=no
```

Three orders of magnitude cheaper than an image, it answers "what does that
label actually say and is that button focused?", and unlike a PNG it diffs.

## Wire format

`shot` emits, all ASCII:

```
SHOT <w> <h> rgb565le rle1
S <x1> <y1> <x2> <y2>
<base64, 76 cols>
S ...
END bytes=.. raw=.. crc32=.. stripes=.. covered=.. elapsed_ms=..
```

A stripe's base64 runs until the next `S` or `END`; the RLE decoder stops when
it has produced exactly that stripe's pixel count. Each stripe carries its own
area rather than assuming top-to-bottom order, and `covered` proves the frame is
whole. `badge.py` checks both the CRC and the coverage before writing a PNG, so
a torn capture fails loudly instead of producing a plausible-looking image.

`rle1` is PackBits over 16-bit pixels. Control byte:

- `0x00..0x7F` — `n+1` literal pixels follow, 2 bytes each, little-endian
- `0x80..0xFF` — run of `n-0x80+2` copies of the single pixel that follows

Measured against synthetic frames: 2.3 KB for a flat screen, ~7 KB for a typical
UI, 29 KB for a full-screen gradient, 200 KB for pure noise (RLE's worst case,
slightly larger than raw).

### Why it looks like this

- **Base64, not raw binary.** The console VFS is `ESP_LINE_ENDINGS_CRLF` on TX,
  so every `\n` the firmware writes goes out as `\r\n`. Raw binary would have
  every `0x0A` byte corrupted. Base64's alphabet avoids that.
- **Streamed, not buffered.** The C3 has no PSRAM. A full frame is 150 KB
  contiguous against ~117 KB of DRAM heap, LVGL runs `PARTIAL` render mode so no
  complete frame exists in RAM at any instant, and the ST7789 is on a no-MISO
  bus so its GRAM can't be read back. `hal_screencap.cpp` forces a full-screen
  invalidate and encodes each stripe as it reaches the flush callback.
- **The UI freezes for a few hundred ms** during a capture, because the whole
  encode runs inside a held LVGL lock. Fine for a dev command; don't call it in
  a tight loop and expect smooth animation.

## Protocol traps

Both of these have bitten this repo before — see `tools/badge-provision/README.md`.

- **Send a bare `\r`.** RX is `ESP_LINE_ENDINGS_CR`, so `\r\n` delivers two line
  terminators and the second lands in the next command.
- **Chunk writes under 256 bytes.** The badge's USB-Serial-JTAG RX ring is 256
  bytes and cannot be grown; a longer burst drops its tail silently.

## Two badges: `duet.py`

`badge.py` is deliberately one device — it takes one `--port` and refuses to
guess when two are plugged in. But pairing, a move crossing the air, and the
radio duty tiers are all statements about a *pair* of badges, and checking them
one badge at a time cannot see the interesting part.

```sh
tools/badge/duet.py a:console "chess bump" b:console "chess bump" settle 2500 \
    a:console "chess" a:expect "mp_state=Live" \
    b:console "chess" b:expect "mp_state=Live" \
    agree session differ seat
```

Every verb except `settle` takes a target prefix: `a:`, `b:` or `both:`. The
explicitness is the point — in a two-badge test, "which badge" is the thing you
are most likely to get wrong, and a default would hide it. `both:shot x.png`
writes `a-x.png` and `b-x.png` so the two cannot clobber each other.

Two verbs exist that make no sense on one badge:

| verb | meaning |
|---|---|
| `agree <key>` | both badges' last output has `<key>=<value>`, and they match |
| `differ <key>` | ...and they do not |

Those turn "the session ids are the same and the seats are opposite" into an
actual comparison rather than a substring match that can pass by accident.
They read `key=value` out of the console dumps, which is why every dump on the
badge side is space-separated `key=value` throughout.

Ports come from `--port-a`/`--port-b`, then `BADGE_PORT_A`/`BADGE_PORT_B`, then
the same glob `badge.py` uses — which needs exactly two matches.

`duet.py` is not a fork. It imports `badge.py` and reuses its `Badge` class and
verb handlers wholesale, so transport fixes land in both at once.

## Troubleshooting

- **"no prompt from …"** — the badge isn't running this firmware, or something
  else owns the port (an `idf.py monitor`, or `tools/serial-buttons`). Only one
  process can hold it.
- **"several ports match"** — pass `--port`.
- **"crc mismatch" / "incomplete frame"** — the capture was cut short. Raise
  `--timeout`; if it repeats, `--debug` dumps the raw console traffic.
- **Colours look inverted** — try `--invert`. The panel runs
  `esp_lcd_panel_invert_color(true)`, which should mean the LVGL buffer holds
  true colour and no host-side inversion is needed, but that flag is there if
  the hardware disagrees.
