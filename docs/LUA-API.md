# Badge Lua API Reference

Lua scripts run as first-class badge apps. Each script lives in its own
directory under `/littlefs/apps/<slug>/` with a `manifest.cfg` and a
`main.lua`, and appears in the launcher like any built-in app.

Upload files with the console (`mkdir` / `put`, which is what badgectl
speaks), then rescan without rebooting:

These are console protocol commands, not shell commands. Create the directory
first; send a bare CR after each command, then exactly the UTF-8 byte count
after `READY`, in chunks smaller than the 256-byte USB receive ring. Prefer the
IDE, which handles byte counts and pacing.

```text
mkdir /littlefs/apps/my_dice
put /littlefs/apps/my_dice/manifest.cfg <size>   # then send <size> bytes
put /littlefs/apps/my_dice/main.lua <size>       # then send <size> bytes
reload
apps
```

`main.lua` must stay at most 64 KiB. The folder basename must equal the
manifest `slug`, or the loader skips it.

For browser upload steps, a copyable agent prompt, and complete single-file apps,
see the [Badge app guide](../tools/badgectl/web/README.md).

## Lifecycle

```lua
function on_enter(root) end          -- initialize UI/state; root is the app screen
function on_tick() end               -- nominal 20 ms cadence while foreground
function on_button(button, kind) end -- button/kind are integers (see badge.input)
function on_exit() end               -- save state; UI is torn down after
```

Lua callbacks share the badge's UI task and lock. Long callbacks prevent
drawing, input processing, HOME handling, and other UI work from progressing.
The execution deadline is a guard against runaway or simply excessive work,
not evidence that the badge is faulty.

Every callback has an elapsed-time budget, including native binding calls
and time spent waiting for other tasks. A Lua instruction hook periodically
checks the deadline and raises an on-screen error card with a traceback.
It cannot interrupt a native call midway, so these are execution allowances,
not precise real-time cutoffs. Leave room below them:

| Callback | Budget | Notes |
|---|---|---|
| initial setup / main chunk | 3,000 ms | `require` shares the calling callback's budget |
| `on_enter` | 3,000 ms | one-shot UI and state setup |
| `on_tick` / queued `on_recv` | 250 ms shared | target a few ms for smooth animation; slower work may drop frames |
| `on_button` | 1,000 ms | allows heavier actions, board updates, and occasional saves |
| `on_exit` | 1,000 ms | save changed state and clean up |

Three consecutive `on_tick` failures suspend tick delivery. A button callback
failure suspends it immediately. In that suspended state, **A** allows another
attempt; it does not restart the app or repair partially changed state. HOME
and reopening give a fresh Lua state. A startup failure closes the Lua state;
fix and reopen the app. Repeated identical errors are rate-limited in the
console and counted on the error card.

These are generous **failure cutoffs, not performance targets**. A bounded
operation may cause a brief pause without making the app fail. Aim for a few
milliseconds of normal tick work so drawing and input remain smooth; a
250 ms tick does not run at 50 frames per second. Prefer a working app over
rejecting a useful feature just because it occasionally drops frames.

Avoid deliberate waiting, busy loops, or unbounded work. There is no Lua
`sleep` API. Use the monotonic clock `badge.sys.ms()` for elapsed time, not a wall-clock date. Do not assume each
tick arrived on time.
`badge.app.exit()`, `badge.nfc.disable()` and `badge.radio.disable()` are
deferred automatically: they take effect after the current callback returns.

The flexible limits above are a firmware change introduced on 2026-09-16.
Earlier firmware has 500 ms for loading, 250 ms for startup (1,000 ms in
the intermediate startup-only update), 6 ms for ticks, 20 ms for buttons,
and 100 ms for exit. Installing an app or refreshing the IDE does not
update firmware, and `api=2` does not identify which limits are installed.
Target the updated allowances; if a user reports a timeout despite bounded
work, check the installed firmware version and account for older limits.
There is no manifest `timeout`, `deadline`, or `budget` option; increasing `heap_kb` or `wake_lock`
does not buy execution time.

For a large board, create the small loading UI in `on_enter`, then create one
cell per `on_tick` until ready. Ignore gameplay input while loading. Set static
styles once, and update only changed cells. Split board resets across ticks
too; moving the whole initialization loop into a single tick only gives it a
smaller budget. Keep each batch bounded and normally far below the 250 ms
tick cutoff; increase batch size only when useful and measured on the badge.

Lua allocations are capped at 48 KiB per app (`heap_kb=96` opts into 96 KiB).
Native widgets use separate memory and are capped at 512 live widgets per app.
Neither number is a performance guarantee or a promise that system/native
memory can accommodate any layout below that cap.

`heap_kb` is an allocation ceiling, not reserved memory. Lua uses the badge's
shared system heap through `realloc`; it also needs room for allocator overhead,
other firmware allocations, and a sufficiently large contiguous block. Growing
an existing block may temporarily require a new block before the old one can
be released. LVGL's widget pool and the 512-widget cap are separate constraints.

Explain this to your user if a log says **"Lua memory limit exceeded" while
`used` and `peak` are below `limit`**. The current firmware uses that same message
for both a quota rejection and a failure of the underlying system allocator.
Neither `used` nor `peak` includes the allocation that failed. Lua can also run
an emergency garbage collection and unwind temporary allocations before the
message is printed. Therefore, those counters alone cannot identify which
allocator check failed; even a quota rejection can occur with `used < limit`
when the next requested allocation would cross the limit.

For example, Packman Maze reported `used 42588 / limit 98304, peak 47745`,
with only `free=59588 largest=49152` immediately before launch. The 96 KiB quota
does not make 96 KiB of physical RAM available. Those numbers point to system
memory pressure during startup, but this older error message does not record
the failed allocation size or distinguish its cause. A later `heap after enter`
line can show the free memory restored because the failed Lua state was already
closed; it does not show the free memory at the instant of failure.

Check the failing phase before changing the app:

- **`main.lua`** includes parsing/compiling the entire file and executing its
  top-level chunk, before `on_enter`. A source file below the 64 KiB upload cap
  can still exceed available RAM while compiling. The submitted Packman app is
  about 14 KiB of source with 32 functions: its incremental maze builder has
  not run at this point. Creating one cell per tick addresses later rendering
  cost, not the compiler's memory peak. Moving function definitions inside
  `on_enter` also does not avoid compiling those nested functions up front.
- For a repeatable startup failure, return a smaller complete app with fewer
  optional behaviors, function bodies, captured locals, and retained tables;
  start with the essential game and add features only after measuring on the
  badge. Removing comments or shortening variable names is not a reliable
  substitute for reducing compiled code and live data. Raising `heap_kb` above
  96 is unsupported, and raising it to 96 cannot fix physical RAM exhaustion.
- **`on_enter` / `on_tick` / `on_button`** failures may instead involve growing
  tables, strings, widget handles, or native widgets. Reuse state and widgets.
  In Packman's LED renderer, the position/color tables are recreated each
  frame; hoist those constants out of the function to reduce later garbage,
  but do not claim that this alone fixes a failure before `on_enter`.
- A fresh reboot can clear memory retained by earlier sessions and reduce
  fragmentation. Treat it as a diagnostic step, not proof that the app fits.
  Ask for `heap` before launch and the first error after launching. In-app
  `badge.sys.stats()` logging only helps if execution reaches that code;
  adding a log to `on_enter` cannot run when compilation has already failed.

Do not tell your user to delete installed apps to fix this RAM error. Installed
source occupies flash storage, and inactive apps do not keep a running Lua VM.

## Manifest (`manifest.cfg`)

`key=value` lines, `#` comments, no inline comments, no duplicate keys.

| Key | Required | Values |
|---|---|---|
| `slug` | yes | `[a-z0-9][a-z0-9_-]{0,31}`, must equal the folder name |
| `name` | yes | 1–48 bytes, shown in the launcher |
| `icon` | no | 1–12 byte text fallback (default `?`) |
| `api` | no | `1` (default) or `2`. `api=2` drops the legacy `badge.label` / `badge.box` factories |
| `heap_kb` | no | `48` (default) or `96` |
| `wake_lock` | no | `0`/`1`. `1` stops the badge sleeping while the app runs |
| `home_button` | no | `0`/`1`. `1` delivers HOME presses instead of exiting (`back_button` is a deprecated alias) |
| `confirm_home` | no | `0`/`1`. Games use `1` to show the shared Home confirmation and pause ticks until confirmed or cancelled. Cannot be combined with `home_button=1`. |
| `version` | no | ≤48 bytes, informational |
| `author` | no | ≤48 bytes, informational |

## Sandbox

Available stdlibs: `base` (without `dofile`, `loadfile`, `load`,
`require`, `pcall`, `xpcall`, `setmetatable`), `table`, `string`, `math`,
`utf8`. `os`, `io`, `package`, `debug` and `coroutine` are absent.
`require` is reinstalled as a sandbox (below). All chunks load in **text
mode** — precompiled bytecode is refused, never executed.

`require("mod")` / `require("pkg.mod")`: names match
`[A-Za-z0-9_]` segments joined by `.`, ≤48 chars. Each maps to
`<appdir>/pkg/mod.lua`. Private per-app module cache, cycle-safe (a
require cycle is an error, not a half-built module), depth 8, 16 modules.

## `badge.ui` — widgets

Screen is 320×240: `badge.ui.screen_width`, `badge.ui.screen_height`.

Factories accept a positional form or a single table form:

```lua
local l = badge.ui.label(root, "hello")
local b = badge.ui.box(root, 292, 196)
local bar = badge.ui.bar(root, 0, 100, 40)      -- parent, min, max, value
local t = badge.ui.box{ parent = root, w = 292, h = 196,
  bg_color = 0x222222, border_width = 1 }
```

Table keys: `parent x y w h align align_x align_y hidden clickable`
plus per-type `text value min max checked options src points`.
Factories: `label box bar arc slider image line button switch checkbox
roller textarea`. (`line` points are `{{x,y},…}` or `{{x=…,y=…},…}`,
≤128 points; `image` `src` is a relative path to an installed `.bin` file
inside this app, e.g. `"icon.bin"`; absolute paths and `A:` are refused.)

Positional factory forms (all names are under `badge.ui`):

| Factory | Arguments |
| --- | --- |
| `label`, `textarea` | `(parent, text)` |
| `box`, `button` | `(parent, width, height)`; add a child label for a caption |
| `bar`, `arc`, `slider` | `(parent, min, max, value)` |
| `image` | `(parent, "icon.bin")`; the file must already exist in this app |
| `line` | `(parent, {{0, 0}, {40, 20}})` |
| `switch` | `(parent, checked_boolean)` |
| `checkbox` | `(parent, text, checked_boolean)` |
| `roller` | `(parent, "first\nsecond")` |

Alignment names: `center`, `top_left`, `top_mid`, `top_right`, `bottom_left`,
`bottom_mid`, `bottom_right`, `left_mid`, `right_mid`. Text is limited to
1,024 bytes per widget text value. `child(i)` uses one-based indices; roller `get_selected()` returns a
zero-based index. There is no `set_selected`, `get_text`, or Lua widget event binding;
handle physical input through `on_button` and keep your state in Lua variables.

Object methods — one handle type, type-checked per call:

```lua
w:set_pos(x, y)  w:set_size(w, h)  w:align("center", dx, dy)
w:parent()  w:child(i)  w:child_count()  w:type()  -- "label", "bar", …
w:hidden(true)  w:clickable(false)  w:bring_to_front()  w:delete()
w:set_text("hi")            -- label, checkbox, textarea only
w:set_value(40)  w:set_range(0, 100)      -- bar, arc, slider
w:set_src(path)             -- image
w:set_points({{0,0},{40,20}})             -- line
w:set_checked(true)  w:get_checked()      -- switch, checkbox
w:set_options("a\nb\nc")  w:get_selected() -- roller
w:set_color(0xff0000)       -- text color on labels, bg elsewhere
w:set_border(0x888888, 2)
w:set_font_size("large")    -- "small" (14), default (16), "large" (20)
```

Handles are non-owning: LVGL owns every widget, so letting a handle get
garbage-collected never deletes the widget. Using a widget after it was
deleted (via `:delete()`, or when its parent was deleted) is a clean
`widget has been deleted` error. `root` is read-only: pass it as the parent for your widgets, but do not
style, resize, reposition, or delete it. Create your own full-screen box
if you need a background. Handles to
the same widget compare equal (`==`).

### Styling

```lua
w:style({ bg_color = 0x222222, radius = 8, text_color = 0xffffff })
w:style({ bg_color = 0x444444 }, "indicator:pressed")
```

Colors are `0xRRGGBB` integers. `text_font` takes `14 16 18 20 22 24`
or `small default large`. Selectors are `main indicator knob items
scrollbar`, optionally with `:pressed :checked :disabled :focused`.
Unknown keys error with a did-you-mean hint.

Supported style keys (also accepted directly in factory tables):

- `bg_color`, `bg_opa`, `color`, `opa`, `radius`.
- `border_color`, `border_opa`, `border_width`.
- `text_color`, `text_opa`, `text_font`, `text_align` (`left`, `center`, `right`).
- `arc_color`, `arc_opa`, `arc_width`; `line_color`, `line_opa`, `line_width`.
- `pad_all`, `pad_top`, `pad_bottom`, `pad_left`, `pad_right`, `pad_row`, `pad_column`.
- `shadow_color`, `shadow_opa`, `shadow_width`, `shadow_spread`,
  `shadow_offset_x`, `shadow_offset_y`.
- `flex_flow`: `row`, `column`, `row_wrap`, or `column_wrap`.

Opacity is 0–255. Widths, padding, radius, and font sizes are integer pixels.
Only the listed properties are bound; arbitrary LVGL style properties error.

Two defaults matter on this dark badge: labels start in the theme text
color (near-white) instead of LVGL's light-background grey, and
`bg_color` implies `bg_opa = 255` — style-stripped boxes are transparent
otherwise, so a bare color would render nothing. Set `bg_opa = 0`
explicitly for a transparent tint, and `text_color` for anything dimmer.

`badge.ui.theme` mirrors the firmware theme: `background panel surface
track border accent accent_detail text text_soft text_muted text_dim`.

## `badge.led` — six RGB LEDs and their physical positions

Use the LEDs as part of the app's design. Each of the six can have its own
colour. **Lua indices are 1-based**; the C++ hardware indices are 0-based, so
do not copy raw C++ indices into an app. `badge.led.count()` returns `6` on
this badge.

Viewed from the **front, with the screen upright** (left/right are the
viewer's left/right, not the wearer's):

```text
          TOP / LANYARD

   1 Upper left       2 Upper right
   6 Middle left      3 Middle right
   5 Bottom left      4 Bottom right

              BOTTOM
```

Useful groups and orders, using Lua indices:

| Pattern | Indices |
| --- | --- |
| Left side, top to bottom | `{1, 6, 5}` |
| Right side, top to bottom | `{2, 3, 4}` |
| Top / middle / bottom pairs | `{1, 2}` / `{6, 3}` / `{5, 4}` |
| Clockwise from upper left | `{1, 2, 3, 4, 5, 6}` |
| Clockwise from upper right | `{2, 3, 4, 5, 6, 1}` |
| Counterclockwise from upper left | `{1, 6, 5, 4, 3, 2}` |

```lua
badge.led.set(1, 255, 0, 0)  -- upper left red; index and RGB are integers
badge.led.set_all(255, 255, 255)
badge.led.clear()            -- all off (still needs show)
badge.led.show()             -- latch to the strip
badge.led.count()            -- 6 on this badge
```

Writes stage; `show()` latches. Apps should `clear()` + `show()` in
`on_exit` (the firmware also zeroes the strip between apps).

RGB channels are integers from 0 to 255, in **red, green, blue** order. Unlike
UI colours, LED calls take three separate channels, not one `0xRRGGBB` value.
Round/clamp computed fades with `math.floor` and `math.min`/`math.max`.
The firmware already applies its brightness curve and physical output cap;
use the full logical 0–255 range without applying that cap a second time.
Very small logical values can look off after that curve. There is no Lua
`brightness()`, `pulse()`, `rainbow()`, or effect scheduler: build effects with
these five calls, `badge.sys.ms()`, and `on_tick`.

Build a frame with `set`/`set_all`/`clear`, then call `show()` once.
Animate with timestamps in `on_tick`, not waits or catch-up loops.
HOME confirmation pauses ticks; exiting stops the app. System sync
confirmation temporarily overrides app colours with three green pulses,
then restores the latest requested frame.

## `badge.sensor`

```lua
local x, y, z = badge.sensor.accel()  -- milligravity; nil + err if unavailable
badge.sensor.shake()                   -- true once per shake (refractory)
badge.sensor.tap()                     -- true once per tap (refractory)
badge.sensor.orientation()             -- flat_up flat_down left_edge
                                       -- right_edge top_edge bottom_edge unknown
```

Reads are cached at 50 Hz and never touch I²C from the Lua task.

## `badge.input`

```lua
badge.input.BUTTON  -- A B HOME DOWN LEFT RIGHT UP AUX1 START
badge.input.KIND    -- PRESSED RELEASED
badge.input.is_down(badge.input.BUTTON.UP)
badge.input.held()  -- bitmask of held buttons
```

Held state is tracked from the Pressed/Released stream. HOME is never
reported held: its Pressed is swallowed by the registry's HOME-button
intercept while its Released still reaches the app. B is an ordinary button
and is always reported.

## `badge.sys`

```lua
badge.sys.ms()       -- monotonic milliseconds (the only clock; os is closed)
badge.sys.uptime()   -- monotonic seconds
badge.sys.log("...")-- one line to serial, tagged with the app slug
badge.sys.random()   -- 32-bit random; badge.sys.random(n) gives 0..n-1
badge.sys.heap()     -- Lua bytes currently used by this app
badge.sys.gc_step()  -- one incremental GC step
badge.sys.version()  -- firmware version string
badge.sys.wake_lock(true)  -- runtime counterpart of manifest wake_lock=
badge.sys.stats()    -- { lua_used lua_peak lua_limit widgets uptime_ms free_heap }
```

The last run's stats also surface in the diagnostics app's `lua:` line.

## `badge.store` — persisted config

Polymorphic over integers and strings, scoped to the app's own slug
(no slug argument, unlike the legacy flat calls). 32 keys, key names
`[A-Za-z0-9_]` ≤24 bytes, strings ≤128 bytes without line breaks:

```lua
badge.store.set("best", 12)  badge.store.get("best", 0)
badge.store.set_int("n", 3)   badge.store.get_int("n", 0)
badge.store.set_str("name", "ada")  badge.store.get_str("name", "?")
```

## `badge.me` — identity without PII

```lua
badge.me.name()  badge.me.role()  badge.me.role_name()
local r, g, b = badge.me.color()
badge.me.badge_id()   -- nil when unprovisioned
badge.me.provisioned()
```

Networking fields (email, phone, socials) are deliberately absent: a
third-party app with radio access must not be able to read and broadcast
them.

## `badge.contacts` — read-only address book

```lua
local n = badge.contacts.count()
local c = badge.contacts.get(1)  -- 1-based; nil + err when out of range
-- c = { name=…, role=…, badge_id=…, received_unix=… }
```

Curated: no contact details beyond name/role/id/timestamp, in sorted
order so indices are stable across calls.

## `badge.app`

```lua
badge.app.slug()  badge.app.name()
badge.app.exit()  -- deferred return to launcher (lands after the callback)
```

## `badge.fs` — sandboxed files

Paths are relative to the app directory; the `appdata/` prefix maps to
`/littlefs/appdata/<slug>/` (private data, wiped by `factory_reset`).
`..`, absolute paths, backslashes and NUL bytes are rejected — there is
no path normalization to get wrong. 64 KiB total quota (app files plus
private appdata), 16 KiB per file for `badge.fs` reads/writes:

```lua
badge.fs.write("appdata/save.dat", "level=2")
badge.fs.append("appdata/log.txt", "won\n")
local s = badge.fs.read("appdata/save.dat")       -- nil + err when missing
badge.fs.exists("appdata/save.dat")  badge.fs.remove("appdata/save.dat")
badge.fs.list()  badge.fs.list("saves")   -- names, no . / ..
badge.fs.mkdir("saves")
```

Binary-safe (length-delimited strings). Removing a missing file returns
false; only regular files can be removed.

## `badge.nfc`

```lua
badge.nfc.enable()   -- true on success; registry disables between apps
badge.nfc.disable()  -- deferred, like badge.app.exit
local c = badge.nfc.card()  -- nil, or { uid="…" hex, sak=…, atqa=… }
local t = badge.nfc.read_text()  -- first NDEF Text record, or nil + err
badge.nfc.clear()    -- forget the last-seen card
```

NFC is power-hungry and off by default; enable in `on_enter`.

## `badge.radio` — restricted Lua channel

Every Lua frame carries a `LUA1` prefix and RX is filtered to it: scripts
can neither emit nor observe system (bump/sync) frames.

```lua
badge.radio.enable()           -- true on success; registry disables between apps
badge.radio.disable()          -- deferred (BLE teardown takes ~2 s)
badge.radio.send("ping")       -- 1–44 byte payload; true on queued
badge.radio.on_recv(function(mac, rssi, payload) end)  -- nil clears
badge.radio.mac()              -- this badge's BLE address, "AA:BB:…"
badge.radio.dropped()          -- frames dropped while the ring was full
```

Reception is an 8-slot ring written from the BLE task and drained ≤4
frames per tick inside the shared 250 ms tick allowance. A full ring drops
frames (visible via `dropped()`); slow receive handlers can still delay the
UI, so keep them short. Payloads arrive without the `LUA1` prefix.

## Legacy compatibility

`badge.label` and `badge.box` factories are available only with `api=1`.
Other older flat helpers (`badge.config_get/set`, `badge.random`,
`badge.gesture_shake/tap`, `badge.button` / `badge.kind`) are currently also
registered in API 2. New apps should set `api=2` and use the namespaced forms
shown above; API 1 remains available for older scripts.

## Limits cheat sheet

48 KB heap (96 via `heap_kb=96`) - 512 widgets - 64 KiB `main.lua` -
32 store keys - 64 KiB fs quota / 16 KiB per file - 44-byte radio payloads -
8-slot radio ring, 4 drained per tick - require depth 8, 16 modules.

## Sharing apps between badges

The Share app (launcher, always visible) lists every installed Lua app —
including ones you didn't write — and sends the whole app to another badge
over Bluetooth. No server or cables are needed.

Tell your user: on the sender, open **Share → Send an app**, pick the app,
then press **A: offer app** on its details screen. Leave that screen open;
it keeps advertising until accepted or cancelled. On the other badge, open
**Share → Receive an app**, review **Incoming app**, then press **A: accept**
once. The transfer starts immediately. No bump, simultaneous presses, or extra
press on the sender is needed. Keep the badges close and still until it finishes.
**B** cancels; declining returns to the Share menu. After installation, the app
appears in the receiver's launcher. Opening Share or Sync from the launcher
briefly reboots directly into that app to start with clean memory. Returning
home after using the radio also reboots to release Bluetooth's memory;
installed apps and saves are preserved.

Rules worth knowing as an app author:

- The bundle is your whole app directory: `manifest.cfg`, `main.lua`,
  `require()` modules, `icon.bin`. Save data under `appdata/` is personal
  and never sent.
- Bundle caps: 48 KiB total, 16 files, paths ≤ 64 bytes and ≤ 4 deep.
  An app bigger than that cannot be shared ("App is too big to share").
- Share streams through temporary files with a 1 KiB copy/verification buffer,
  so neither badge needs the whole bundle in RAM. Radio packets carry smaller
  213-byte fragments; missing fragments are retried. The receiver checks the
  entire bundle's CRC before reporting completion and installing it.
- Leave free storage for staging: one bundle on the sender; the bundle plus
  unpacked files on the receiver (roughly twice the bundle size). Failed or
  cancelled transfers remove their temporary files. Existing apps stay in
  place until the received app has passed validation. A fresh badge does
  not need any apps preinstalled: Share creates its apps directory automatically.
- The receiver's badge runs the exact same manifest validation as boot
  (`slug` must equal the folder name, `main.lua` present, sizes sane). If
  your app loads at boot, it survives the trip.
- Slug collisions ask: keep both (installed as `<slug>-2`), overwrite, or
  cancel. A slug matching a built-in app is refused outright.
- Received code runs in the same sandbox with the same quotas — sharing
  your app never grants it more power on someone else's badge.
