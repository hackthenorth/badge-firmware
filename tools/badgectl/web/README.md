# Agent instructions: create a Hacker Badge app in the IDE

You are helping a user make a Lua app for their 2026 Hacker Badge. Treat this
Markdown file as your app-development brief: it contains the IDE workflow,
the supported APIs, runtime limits, and complete examples. Work through the
[Badge IDE](https://badge.hackthenorth.com/ide/). Everything needed to write the
app is documented here; repository access, terminal commands, and development
tools are not required.

Your job is to understand what the user wants, write the whole app, and explain
how they can put your answer into the IDE and run it on their badge.

## Check which IDE the user has

The current **Hacker Badge Custom Apps** IDE provides **Import app**,
**Download app**, and **Download guide**. **Check the user's page before
naming buttons**: an older or cached page may show only **Connect**, **Push**,
**Reboot**, and **^C**. Inspect it if you have browser access. Otherwise, explain both
paths below instead of telling the user that a missing button exists.

On a page without **Import app**, the user must fill the two existing editor
files manually. Still deliver one complete config-and-code block, and tell
them exactly how to split it using the manifest header. A missing Import app
button is a page-version difference, not a badge or USB problem.

## Start with the user's idea

If the user has not told you what to make, **ask them before writing an app**.
Tell them you can build it for them. For example:

> What would you like to make for your badge? I can build a game, a timer,
> a light effect, or an app that reacts to movement. Tell me your idea and
> I'll give you the complete app to paste into the badge IDE.

Wait for their answer. Do not choose an example and present it as their app.
If they already gave an idea, use it; do not ask them to repeat it. Ask a short
follow-up only when a missing detail materially changes the app. Otherwise,
choose sensible controls and defaults and build it.

Use the capability table and API reference below to check feasibility. If a
requested feature is unavailable, explain that specific limit and ask the
user to choose an achievable alternative. Once the idea is clear, return a
complete app, not just a plan or an offer to write it later.

## What to deliver to the user

1. Briefly describe what the app does, its button controls, and what it saves.
   Explain what its LED colours/patterns mean, including any position-specific
   cues and brightness or effects controls.
2. Return **exactly one fenced `lua` code block** containing the full manifest
   header followed by the entire runnable app. Use the single-file format below.
   Include every helper function. Do not split config and code across blocks,
   omit sections, supply a diff, or require extra files or installation scripts.
3. Explain the installation path that matches their IDE. If **Import app** is
   present, they can paste the complete code block there. If it is absent,
   explain how to put the header contents in `manifest.cfg` and the app code
   in `main.lua`, as described below. Then explain **Connect → Push**.
4. State what you actually checked. Do not claim that an app was uploaded or
   tested on a physical badge unless you verified that result.
5. For a large UI or game, account for initialization, normal moves, wins,
   resets, and saving separately. Explain any brief loading phase and when
   progress is saved. Supported APIs and a low widget count do not establish
   that callbacks meet their deadlines on real hardware.

Keep explanation outside the code block. Put short control instructions in Lua
comments and in the app's UI too. If the user requests changes later, return the
complete updated single-file app again so they can replace it in one paste.
Keep its slug the same when updating the same app.

If you have browser tools, you can help prepare the app in the IDE. If you do
not, the same complete code block and user instructions are sufficient. The
user must have their badge connected to use Push; generating code alone does
not install or run it.

## What to tell the user about installing the app

Give the user the path that matches their page. If you cannot inspect the
page, make the two alternatives explicit.

### Older page without Import app

1. In the IDE's left **files** list, click **manifest.cfg**. Replace its contents
   with just the `key=value` lines between `--[==[badge-app` and `]==]` in your
   response. Do **not** include either delimiter or the Markdown code fences.
2. Click **main.lua** in the same list. Replace its contents with all the Lua
   code after `]==]`. Do not include the config header or Markdown fences.
3. Turn the badge off, connect a USB data cable, then turn it on normally
   without holding Start. Close other serial connections to that badge.
4. Click **Connect**, choose **USB JTAG/serial debug unit** (Espressif), then
   click **Push**. Wait for the upload to finish.
5. Open the app from the badge launcher with **A**. Explain its controls.

Tell the user to save their previous code/config before replacing editor
contents. Do not suggest Download app unless that button exists on their page.

### Page with Import app

Use these steps only when **Import app** is visible:

1. Open the [Badge IDE](https://badge.hackthenorth.com/ide/) in desktop Chrome
   or Edge. Use a USB **data** cable and close other tabs/tools using the badge's
   serial port.
2. Save any current work first. **Download app** saves a workspace containing
   only config and code as one `.lua` file. Extra modules/images need to be
   saved separately.
3. Click **Import app**, paste the whole Lua code block including its manifest
   header, or choose the saved `.lua` file. A single surrounding Markdown code
   fence is accepted; do not paste the explanatory text around the block.
4. Check the slug in the preview, then click **Replace editor files**. This
   replaces the browser workspace with `manifest.cfg`, `main.lua`, and this
   guide. It has not changed the badge yet.
5. Turn the badge off, plug in USB, then turn it on normally. **Do not hold Start**.
6. Click **Connect**, follow the guide, and choose **USB JTAG/serial debug unit**
   (sometimes labelled Espressif) in the browser's device picker.
7. Click **Push** and keep the cable connected until the upload completes.
8. Find the app in the badge launcher and press **A** to open it. Explain the
   app's controls. **HOME** normally returns to the launcher.

Explain the editor controls when relevant: **Import** changes the browser
workspace, **Push** installs the app files, and **Reboot** restarts the badge.
The IDE is an editor/uploader, not an on-screen badge simulator. It writes files
under `/littlefs/apps/<slug>/` and refreshes the launcher with `reload`.
`README.md` stays in the browser and is never uploaded to the badge.

The browser saves drafts locally every two seconds and on Ctrl/Cmd+S. Explain
that this is not a download or an upload. Recommend saving a copy before
clearing browser storage or switching computers. On the updated page,
**Download app** exports the app and **Download guide** saves these agent
instructions. The updated page also refreshes this guide when it opens.

### Other IDE features you can explain when needed

Keep generated apps self-contained by default. For a user who specifically
wants multiple files, config can be edited in `manifest.cfg` and code in
`main.lua`. **+** adds files such as `util.lua` or `lib/mathutil.lua`, loaded
with `require("util")` or `require("lib.mathutil")`. Every file except
`README.md` is pushed, so tell the user to keep unrelated files out of the
workspace.

**Choose image** crops/resizes an image to a 42×42 `icon.bin` (5,304 bytes,
LVGL RGB565A8), overriding the text icon. The single-file format contains text
config and code only. Use a short ASCII text icon for the app you deliver;
adding an image through the IDE is an optional user step.

A new slug installs another app; the same slug overwrites that app's uploaded
files. Choose a unique slug, e.g. `my_reaction`, instead of a built-in slug
such as `dice` or `reaction`. Push does not remove remote files deleted from
the editor. If the user wants to remove an old image, explain that the IDE
console command `rm /littlefs/apps/THEIR_SLUG/icon.bin`, followed by `reload`,
removes that exact file from the badge. Substitute their actual app slug.

Reopening an app starts a fresh Lua state and reads the new `main.lua`.
Tell the user to **Reboot after changing runtime manifest options on an
already-installed slug** (`api`, `heap_kb`, `wake_lock`, `home_button`,
`confirm_home`): a rescan only refreshes its name and icon. Ordinary code edits
need Push and reopening the app, not a reboot.

## The single-file format

An app file begins with a Lua long comment. The importer extracts its contents
as `manifest.cfg` and everything after it as `main.lua`. The header delimiters
must appear exactly as shown, each on its own line. This is a packaging format
for the IDE; the badge still stores two files. Pasting the combined file into
just the `main.lua` editor does **not** update the manifest.

```lua
--[==[badge-app
slug=my_hello
name=My Hello
icon=HI
api=2
]==]

function on_enter(root)
  local label = badge.ui.label(root, "Hello, badge!")
  label:align("center", 0, 0)
end
```

Import validates the header, manifest values, and code size. Lua syntax and
hardware behaviour are checked when the user opens the app on the badge. An imported
app should have no missing modules, external assets, or internet dependencies.

## Rules for the app you generate

Use only APIs documented in this guide. Do not invent raw LVGL, Arduino,
MicroPython, Love2D, Wi-Fi, HTTP, audio, touch, or browser APIs inside Lua.

- Set `api=2`, choose a unique lowercase slug and a short ASCII icon, and
  normally use `heap_kb=48`. Include all helpers in the file, with no
  `require()` dependencies, external assets, or internet dependencies.
- Design for 320×240 and physical buttons. Use global lifecycle functions,
  `badge.input` constants, integer widget coordinates, and readable labels.
- Create the UI in `on_enter` and reuse its widgets. Prefer building large
  grids in small steps across `on_tick` calls; block gameplay until setup is
  complete. A bounded pause is acceptable under the flexible deadlines below.
  Use `badge.sys.ms()` for timers and respect the separate elapsed-time,
  Lua heap, and native widget limits.
- Use HOME's default exit unless the requested app needs a custom HOME flow.
  Use `wake_lock=1` only when the foreground app needs to stay awake.
- Cache saved values in memory; write only changed values on explicit actions
  or `on_exit`, never on every tick. Flash writes also take callback time.
  If saving on exit, tell the user to leave via HOME before switching off;
  a power cut or startup failure does not guarantee `on_exit` will run.
  Handle unavailable sensors and failed NFC/radio enable.
- Make generous, purposeful use of the six RGB LEDs wherever the idea benefits
  from them. Add effects for relevant app states and actions, not just one
  static colour on launch. Use the physical layout and design suggestions
  below; keep essential information on the screen too. Clear/show in `on_exit`.
  Radio messages need an app-specific prefix and must fit in 44 bytes.
- Before replying, check every API call, restart/exit paths, text placement,
  and that the full `main.lua` is at most 64 KiB. Use the complete examples as
  patterns, adapting the code to the user's idea.

## Badge identity you can use in an app

`badge.me.badge_id()` returns the badge's assigned ID, or `nil` when it has not
been provisioned. Read it at runtime; do not ask the user to look up their ID
just to generate an app, and do not hardcode an example person's ID. Handle
absence gracefully, for example with `badge.me.badge_id() or "No badge ID"`.

`badge.me.name()`, `badge.me.role_name()`, and `badge.me.color()` can personalize
an app's greeting and colours. Use identity only when it helps the user's idea.
Tell the user if their requested feature uses or broadcasts their badge ID.

The badge ID is distinct from the app's manifest `slug` and from
`badge.radio.mac()`, which is a radio address. The ID is not a password or proof
of authentication. The Lua API exposes no email, phone, or social-account
fields. See `badge.me` and `badge.contacts` below for the exact available calls.

## What you can make

| Capability | Examples | Boundary |
| --- | --- | --- |
| Screen + buttons | Games, menus, scoreboards, timers, quizzes | 320×240; widget bindings, no raw LVGL or canvas API |
| Motion | Tilt level, shake dice, movement games | Cached accelerometer readings; handle unavailable hardware |
| LEDs | Player colours, directional cues, progress, chases, fades, celebrations | Six individually addressable RGB LEDs; stage a frame, then call `show()` |
| Storage | Best scores, settings, saved game data | Your own app's store/files only |
| Badge identity | Personal greeting, role-colour UI | Curated name/role/ID; no private networking fields |
| Contacts | Contact counts and name lists | Read-only curated records; no email/phone/social fields |
| NFC | UID viewer, NDEF text reader | Reader only; no Lua tag-writing API |
| Nearby radio | Two-badge games, short messages | Restricted Lua broadcast channel, no Wi-Fi/HTTP or system frames |
| Sharing | Send installed Lua apps through Share | App directory must fit Share's file and size caps |

There is no exposed Lua audio, arbitrary BLE/GATT, network fetch, task/thread,
or touchscreen event API. Visual `button`, `slider`, `roller`, etc. widgets do
not provide Lua click/change handlers: handle physical input in `on_button`
and update the widget state explicitly. Apps run only while in the foreground;
a timer or radio listener does not keep running after returning HOME.

## Runtime and API reference

### Lifecycle

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
does not buy execution time. Keep your workflow in the IDE: optimize the app
and explain a firmware-version dependency if relevant, rather than giving
the user a separate firmware-installation workflow.

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

### Design every expensive path, not just startup

For a board game, use these patterns when generating the complete app:

1. Create status labels and the board container in `on_enter`. Keep a build
   index and create/style one cell per tick initially; return immediately
   after that unit of work. Keep the loading label/cursor consistent and
   ignore gameplay input until all cells exist. One cell is a starting point,
   not a guarantee: simplify it further if device measurements demand it.
2. Set geometry, radius, font, and borders once. On a move, update only the
   changed cells and labels. Avoid setting unchanged text or style tables in
   every frame. Check a new win through the last move where the rules allow.
3. On reset, clear the logical board and schedule visual updates in small
   steps. On a win, schedule any large highlight/animation the same way.
   Cancelling or starting a new round must discard pending work from the old
   round. A bounded whole-board update is allowed if it fits the button
   allowance, but do not assume it will fit just because it ran during the
   larger startup allowance. Prefer reusing widgets over recreating them.
4. Keep AI/search bounded by both depth and work per tick, carrying progress
   in Lua tables. Cap catch-up steps after delays. There is no exposed thread,
   coroutine, sleep, or timer-callback API to move blocking work into.
5. Keep score/settings changes in memory and save only when needed. A small
   game may save changed scores in `on_exit`'s 1,000 ms allowance; larger saves
   need a bounded design too. Describe the persistence tradeoff to the user.

A 42-cell Connect 4 board hit the earlier deadlines: creating/styling 42 cells
and immediately restyling all 42 during `new_round()` duplicates startup work,
and winning/resetting may combine a full repaint with a flash write. The
flexible limits give these operations more room. Widget reuse, fewer native
calls, and batching still help responsiveness; do not mistake a smooth-frame
target for the firmware's failure cutoff.

### Manifest (`manifest.cfg`)

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

### Sandbox

Available stdlibs: `base` (without `dofile`, `loadfile`, `load`,
`require`, `pcall`, `xpcall`, `setmetatable`), `table`, `string`, `math`,
`utf8`. `os`, `io`, `package`, `debug` and `coroutine` are absent.
`require` is reinstalled as a sandbox (below). All chunks load in **text
mode** — precompiled bytecode is refused, never executed.

`require("mod")` / `require("pkg.mod")`: names match
`[A-Za-z0-9_]` segments joined by `.`, ≤48 chars. Each maps to
`<appdir>/pkg/mod.lua`. Private per-app module cache, cycle-safe (a
require cycle is an error, not a half-built module), depth 8, 16 modules.

### `badge.ui` — widgets

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

#### Styling

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

### `badge.led` — six RGB LEDs and their physical positions

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

#### Encourage expressive, relevant LED effects

When generating an app, proactively choose several effects that make its
state easier to read or make it more fun. Explain those choices to the user:

- **Games:** player-coloured sides, a short move/score pulse, a rejected-move
  cue, and a victory chase or colour wave. For Connect 4, the left three LEDs
  can represent P1 and the right three P2; pulse the active side and celebrate
  with the winner's colour. Keep the turn/result visible on screen too.
- **Timers/progress:** a six-step countdown or progress fill, a slow waiting
  pulse, and a distinct completion sequence. Choose the physical fill order
  explicitly; numeric order travels around the perimeter, not down both sides.
- **Movement/navigation:** illuminate the left/right side or top/bottom pair
  matching the app's direction, and show a centred/level success colour. Match
  the actual screen movement; do not assume a raw accelerometer sign means left.
- **NFC/radio:** searching/waiting animation, a brief read/receive pulse, and
  clear success/failure feedback. Sending a radio packet only means queued;
  do not signal confirmed delivery unless your protocol receives an acknowledgement.
- **Ambient/light apps:** colour chases, breathing, gradients, paired sweeps,
  and interactive colour/brightness controls. Use all six positions when
  useful; allow the user to reduce brightness or switch effects off.

Prefer fades, travelling light, and brief event pulses over constant rapid
flashing. Pick a consistent colour language within each app. An event effect
should finish and return to the current idle/turn/progress display; a new round
or mode must cancel any stale victory/error animation.

Build each frame by clearing or overwriting all affected LEDs, then call
`show()` **once**. Advance effects over ticks using timestamps (e.g. 50 ms
animation updates or 150 ms chase steps), never a loop that waits for an
animation to finish. Skip missed frames after a delay instead of replaying
them in a large catch-up loop. During HOME confirmation, ticks pause, so
derive the next frame from the current time. Apps stop animating after exit.
System sync confirmation can temporarily show three green pulses, then
restore the latest app-requested colours.

### `badge.sensor`

```lua
local x, y, z = badge.sensor.accel()  -- milligravity; nil + err if unavailable
badge.sensor.shake()                   -- true once per shake (refractory)
badge.sensor.tap()                     -- true once per tap (refractory)
badge.sensor.orientation()             -- flat_up flat_down left_edge
                                       -- right_edge top_edge bottom_edge unknown
```

Reads are cached at 50 Hz and never touch I²C from the Lua task.

### `badge.input`

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

### `badge.sys`

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

### `badge.store` — persisted config

Polymorphic over integers and strings, scoped to the app's own slug
(no slug argument, unlike the legacy flat calls). 32 keys, key names
`[A-Za-z0-9_]` ≤24 bytes, strings ≤128 bytes without line breaks:

```lua
badge.store.set("best", 12)  badge.store.get("best", 0)
badge.store.set_int("n", 3)   badge.store.get_int("n", 0)
badge.store.set_str("name", "ada")  badge.store.get_str("name", "?")
```

### `badge.me` — identity without PII

```lua
badge.me.name()  badge.me.role()  badge.me.role_name()
local r, g, b = badge.me.color()
badge.me.badge_id()   -- nil when unprovisioned
badge.me.provisioned()
```

Networking fields (email, phone, socials) are deliberately absent: a
third-party app with radio access must not be able to read and broadcast
them.

### `badge.contacts` — read-only address book

```lua
local n = badge.contacts.count()
local c = badge.contacts.get(1)  -- 1-based; nil + err when out of range
-- c = { name=…, role=…, badge_id=…, received_unix=… }
```

Curated: no contact details beyond name/role/id/timestamp, in sorted
order so indices are stable across calls.

### `badge.app`

```lua
badge.app.slug()  badge.app.name()
badge.app.exit()  -- deferred return to launcher (lands after the callback)
```

### `badge.fs` — sandboxed files

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

### `badge.nfc`

```lua
badge.nfc.enable()   -- true on success; registry disables between apps
badge.nfc.disable()  -- deferred, like badge.app.exit
local c = badge.nfc.card()  -- nil, or { uid="…" hex, sak=…, atqa=… }
local t = badge.nfc.read_text()  -- first NDEF Text record, or nil + err
badge.nfc.clear()    -- forget the last-seen card
```

NFC is power-hungry and off by default; enable in `on_enter`.

### `badge.radio` — restricted Lua channel

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

### Legacy compatibility

`badge.label` and `badge.box` factories are available only with `api=1`.
Other older flat helpers (`badge.config_get/set`, `badge.random`,
`badge.gesture_shake/tap`, `badge.button` / `badge.kind`) are currently also
registered in API 2. New apps should set `api=2` and use the namespaced forms
shown above; API 1 remains available for older scripts.

### Limits cheat sheet

48 KB heap (96 via `heap_kb=96`) - 512 widgets - 64 KiB `main.lua` -
32 store keys - 64 KiB fs quota / 16 KiB per file - 44-byte radio payloads -
8-slot radio ring, 4 drained per tick - require depth 8, 16 modules.

### Sharing apps between badges

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
- If your user reports "Radio wouldn't start", explain that Bluetooth still
  needs its own RAM even with streaming. Ask them to reboot and retry Share.
  If it persists, collect the console's `radio` output and firmware version:
  older firmware can exhaust Bluetooth's advertising memory even for a small
  app. Do not treat a successful USB Push as proof that radio startup works.
  Only **"Storage is full"** means the filesystem reported no space; then ask
  them to free storage. **"Couldn't access app files"** means a file operation
  failed: collect the `share_xfer` console error, its `errno`, and firmware
  version. Older firmware wrongly said "Free some storage" when a fresh badge
  had no `/littlefs/apps` directory. Updating the badge fixes that first-app
  receive bug; do not tell someone with an empty badge to delete apps.
  **"Not enough memory"** refers to RAM: exit Share and retry after the badge
  reboots. For a checksum failure, keep the badges close and retry. Do not
  increase the Lua heap quota to fix a Share radio/storage error.
- If a badge still says **"BUMP TO CONFIRM"** or **"BOTH PRESS A"**, it has the
  older Share flow. Update both badges for these one-accept instructions;
  pressing A again on an old sender can start its old mutual-press handshake.
- The receiver's badge runs the exact same manifest validation as boot
  (`slug` must equal the folder name, `main.lua` present, sizes sane). If
  your app loads at boot, it survives the trip.
- Slug collisions ask: keep both (installed as `<slug>-2`), overwrite, or
  cancel. A slug matching a built-in app is refused outright.
- Received code runs in the same sandbox with the same quotas — sharing
  your app never grants it more power on someone else's badge.

### Practical rules for reliable apps

- Define lifecycle callbacks globally, not as `local function on_enter`. Each
  entry creates a fresh state; only store/files survive closing the app.
- `badge.ui` is the complete drawing interface; C++ examples may use APIs that
  are not exposed to Lua. Use `:` for widget methods and `.` for badge APIs.
- Coordinates/dimensions must be integers. Round accelerometer calculations
  with `math.floor`. Keep content inside the screen, including footer labels.
- Create and reuse widgets. Garbage-collecting a handle does not delete a widget.
  Root is borrowed and read-only; a child box can supply a custom background.
- For a grid, separate initialization, move rendering, reset rendering, and
  animations. Track pending cell indices and return between small units of
  work. Test repeated resets and wins as well as the initial screen.
- Store RGB as `0xRRGGBB`; use fonts 14, 16, 18, 20, 22, or 24. A text icon is
  not an emoji renderer; short ASCII text is the most portable fallback.
- Prefer plain ASCII punctuation in on-screen labels too. The bundled fonts
  do not contain em dashes, curly quotes, or general emoji. If your user sees
  a square in a label, explain that it can be a missing font glyph; replace
  the unsupported character with a hyphen, straight quote, or a line break.
- No sleeping, busy waits, endless loops, or large catch-up loops. Work a little
  each tick. Cache persistent values; do not read/write flash at frame rate.
- `confirm_home=1` pauses tick delivery while its confirmation is open, but
  `badge.sys.ms()` keeps advancing. Lua has no confirmation-cancel callback.
  Choose default HOME exit for elapsed-time games, or implement pause/resume
  explicitly and account for paused time.
- NFC/radio start disabled. Check `enable()` and handle failure. NFC text reads
  involve hardware work; do not repeat them in every tick. The UID example
  below uses the last-seen card cache.
- Radio send success means queued, not received. Frames may be lost or repeated.
  Add a short protocol prefix, validate input, rate-limit transmissions, and
  use sequence numbers/acknowledgements for games that require reliable state.
  Do not unregister the receive callback inside itself: several frames may
  already be scheduled for that tick. Disable on exit instead.

## Complete single-file apps

Each block below is a whole app, including config, that the user can paste into
**Import app**. They use unique `demo_` slugs and require no extra files. Adapt
them to the user's request and return the entire resulting app. The descriptions
show the controls and behaviour you should explain to the user.

### 1. Saved counter — buttons and persistence

**A** adds one; **B** subtracts one; **Start** resets; **HOME** exits and saves.
Saving on exit avoids a flash write for every button press. Power loss before
exit loses that session's changes.

```lua
--[==[badge-app
slug=demo_counter
name=Saved Counter
icon=+1
api=2
heap_kb=48
]==]

local count = 0
local label
local dirty = false

function on_enter(root)
  count = badge.store.get_int("count", 0)
  local title = badge.ui.label(root, "Saved Counter")
  title:align("top_mid", 0, 16)
  label = badge.ui.label(root, tostring(count))
  label:style({text_font = 24})
  label:align("center", 0, -10)
  local hint = badge.ui.label(root, "A +1   B -1   Start reset")
  hint:align("bottom_mid", 0, -40)
  local save = badge.ui.label(root, "HOME saves and exits")
  save:align("bottom_mid", 0, -14)
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then
    count = math.min(999999, count + 1)
  elseif button == B.B then
    count = math.max(0, count - 1)
  elseif button == B.START then
    count = 0
  else
    return
  end
  dirty = true
  label:set_text(tostring(count))
end

function on_exit()
  if dirty then badge.store.set_int("count", count) end
end
```

### 2. Reaction game — timed state machine, LEDs, best score

**A** arms the game. Wait for green, then press **A** again. An early press
is a false start. **B** resets to idle. **HOME** exits immediately. Uses elapsed
time rather than assuming a fixed frame rate; button sampling limits precision.

```lua
--[==[badge-app
slug=demo_reaction
name=Reaction Game
icon=GO
api=2
heap_kb=48
wake_lock=1
]==]

local phase = "idle"
local deadline, go_at, best = 0, 0, 0
local message, record

local function lights(r, g, b)
  badge.led.set_all(r, g, b)
  badge.led.show()
end

local function show_best()
  record:set_text(best > 0 and ("Best: " .. best .. " ms") or "No best yet")
end

local function idle()
  phase = "idle"
  message:set_text("A to start")
  message:set_color(0xffffff)
  lights(0, 0, 0)
end

function on_enter(root)
  best = badge.store.get_int("best_ms", 0)
  local title = badge.ui.label(root, "Reaction Game")
  title:align("top_mid", 0, 16)
  message = badge.ui.label(root, "")
  message:style({text_font = 24})
  message:align("center", 0, -20)
  record = badge.ui.label(root, "")
  record:align("center", 0, 24)
  local hint = badge.ui.label(root, "A start/react   B reset   HOME exit")
  hint:style({text_font = 14})
  hint:align("bottom_mid", 0, -16)
  show_best()
  idle()
end

function on_tick()
  if phase == "wait" and badge.sys.ms() >= deadline then
    phase = "go"
    message:set_text("GO! Press A")
    message:set_color(0x44ff66)
    lights(0, 96, 0)
    go_at = badge.sys.ms()
  end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.B then idle(); return end
  if button ~= B.A then return end
  if phase == "idle" or phase == "done" then
    phase = "wait"
    deadline = badge.sys.ms() + 1000 + badge.sys.random(2501)
    message:set_text("Wait for green...")
    message:set_color(0xff6666)
    lights(96, 0, 0)
  elseif phase == "wait" then
    phase = "done"
    message:set_text("Too soon! A retries")
    lights(0, 0, 0)
  elseif phase == "go" then
    local elapsed = badge.sys.ms() - go_at
    phase = "done"
    message:set_text(elapsed .. " ms - A retries")
    message:style({text_font = 20})
    lights(0, 0, 0)
    if best == 0 or elapsed < best then
      best = elapsed
      badge.store.set_int("best_ms", best)
      show_best()
    end
  end
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
```

### 3. Tilt level — accelerometer, bounded movement, calibration

Tilt to move the bubble; **A** treats the current position as level.
**B** clears calibration; **HOME** exits. Calibration is for this session.

```lua
--[==[badge-app
slug=demo_level
name=Tilt Level
icon=LV
api=2
heap_kb=48
wake_lock=1
]==]

local bubble, readout
local zero_x, zero_y = 0, 0
local next_update = 0

local function clamp(n, lo, hi)
  return math.max(lo, math.min(hi, n))
end

function on_enter(root)
  local title = badge.ui.label(root, "Tilt Level")
  title:align("top_mid", 0, 8)
  local frame = badge.ui.box(root, 200, 140)
  frame:set_pos(60, 42)
  frame:style({bg_color = 0x20252d, border_color = 0x888888,
               border_width = 2, radius = 8})
  bubble = badge.ui.box(root, 20, 20)
  bubble:style({bg_color = 0x33ccff, radius = 10})
  bubble:set_pos(150, 102)
  readout = badge.ui.label(root, "Reading sensor...")
  readout:style({text_font = 14})
  readout:align("bottom_mid", 0, -32)
  local hint = badge.ui.label(root, "A calibrate   B reset   HOME exit")
  hint:style({text_font = 14})
  hint:align("bottom_mid", 0, -10)
end

function on_tick()
  local now = badge.sys.ms()
  if now < next_update then return end
  next_update = now + 100
  local x, y = badge.sensor.accel()
  if not x then
    bubble:hidden(true)
    readout:set_text("Accelerometer unavailable")
    return
  end
  bubble:hidden(false)
  x, y = x - zero_x, y - zero_y
  local dx = clamp(math.floor(-x * 0.08), -84, 84)
  local dy = clamp(math.floor(y * 0.06), -54, 54)
  bubble:set_pos(150 + dx, 102 + dy)
  readout:set_text(string.format("x %d mg   y %d mg", math.floor(x), math.floor(y)))
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then
    local x, y = badge.sensor.accel()
    if x then zero_x, zero_y = x, y end
  elseif button == B.B then
    zero_x, zero_y = 0, 0
  end
end
```

### 4. Pocket light — colours, brightness, LED cleanup

**A** toggles; **Left/Right** change colour; **Up/Down** change brightness.
**HOME** turns LEDs off and saves the selected colour/brightness.

```lua
--[==[badge-app
slug=demo_light
name=Pocket Light
icon=LED
api=2
heap_kb=48
wake_lock=1
]==]

local colors = {{255,255,255}, {255,70,0}, {0,255,100}, {0,100,255}}
local names = {"White", "Warm", "Green", "Blue"}
local selected, level = 1, 64
local lit = true
local label

local function apply()
  if lit then
    local c = colors[selected]
    badge.led.set_all(math.floor(c[1] * level / 255),
                      math.floor(c[2] * level / 255),
                      math.floor(c[3] * level / 255))
  else
    badge.led.clear()
  end
  badge.led.show()
  label:set_text(lit and (names[selected] .. " / " .. level) or "Off")
end

function on_enter(root)
  selected = math.max(1, math.min(#colors, badge.store.get_int("color", 1)))
  level = math.max(8, math.min(255, badge.store.get_int("level", 64)))
  local title = badge.ui.label(root, "Pocket Light")
  title:align("top_mid", 0, 16)
  label = badge.ui.label(root, "")
  label:style({text_font = 24})
  label:align("center", 0, -10)
  local controls = badge.ui.label(root, "A on/off   L/R colour\nUp/Down brightness   HOME exit")
  controls:style({text_font = 14, text_align = "center"})
  controls:align("bottom_mid", 0, -16)
  apply()
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then lit = not lit
  elseif button == B.LEFT then selected = (selected - 2) % #colors + 1
  elseif button == B.RIGHT then selected = selected % #colors + 1
  elseif button == B.UP then level = math.min(255, level + 16)
  elseif button == B.DOWN then level = math.max(8, level - 16)
  else return end
  apply()
end

function on_exit()
  badge.led.clear()
  badge.led.show()
  badge.store.set_int("color", selected)
  badge.store.set_int("level", level)
end
```

### 5. LED tour — position mapping, chase, breathing, and brightness

This app starts a clockwise cyan chase at the upper right. **A** cycles Chase,
Breathe, and Locate. **Left/Right** enter Locate and select one physical LED;
the screen names it. **Up/Down** adjust brightness; **B** toggles the lights.
**HOME** clears them. Settings are session-only. Adapt these effects to events
in the user's app rather than making every app a standalone light show.

```lua
--[==[badge-app
slug=demo_led_tour
name=LED Tour
icon=LED
api=2
heap_kb=48
wake_lock=1
]==]

local clockwise = {2, 3, 4, 5, 6, 1}
local positions = {"Upper left", "Upper right", "Middle right",
                   "Bottom right", "Bottom left", "Middle left"}
local modes = {"Chase", "Breathe", "Locate"}
local mode, head, brightness = 1, 1, 160
local lit = true
local next_frame, next_step = 0, 0
local status, location

local function describe()
  status:set_text((lit and modes[mode] or "Off") ..
                  " / brightness " .. brightness)
  local index = clockwise[head]
  location:set_text(mode == 2 and "All six LEDs" or
                    ("LED " .. index .. ": " .. positions[index]))
end

local function render(now)
  badge.led.clear()
  if lit then
    if mode == 2 then
      local phase = (now % 2000) / 1000
      local wave = phase <= 1 and phase or (2 - phase)
      local level = math.floor(brightness * (0.25 + 0.75 * wave))
      badge.led.set_all(0, level, level)
    else
      badge.led.set(clockwise[head], 0, brightness, brightness)
    end
  end
  badge.led.show()
end

function on_enter(root)
  local title = badge.ui.label(root, "LED Tour - front view")
  title:align("top_mid", 0, 12)
  local diagram = badge.ui.label(root, "1     2\n6     3\n5     4")
  diagram:style({text_font = 20, text_align = "center"})
  diagram:align("top_mid", 0, 42)
  status = badge.ui.label(root, "")
  status:align("top_mid", 0, 118)
  location = badge.ui.label(root, "")
  location:align("top_mid", 0, 143)
  local hint = badge.ui.label(root,
    "A effect   L/R locate   B on/off\nUp/Down brightness   HOME exit")
  hint:style({text_font = 14, text_align = "center"})
  hint:align("bottom_mid", 0, -8)
  local now = badge.sys.ms()
  next_frame, next_step = now + 50, now + 150
  describe()
  render(now)
end

function on_tick()
  if not lit or mode == 3 then return end
  local now = badge.sys.ms()
  if now < next_frame then return end
  next_frame = now + 50
  if mode == 1 then
    if now < next_step then return end
    next_step = now + 150
    head = head % #clockwise + 1
    describe()
  end
  render(now)
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  local B = badge.input.BUTTON
  if button == B.A then mode = mode % #modes + 1
  elseif button == B.LEFT then mode = 3; head = (head - 2) % #clockwise + 1
  elseif button == B.RIGHT then mode = 3; head = head % #clockwise + 1
  elseif button == B.UP then brightness = math.min(255, brightness + 16)
  elseif button == B.DOWN then brightness = math.max(32, brightness - 16)
  elseif button == B.B then lit = not lit
  else return end
  local now = badge.sys.ms()
  next_frame, next_step = now + 50, now + 150
  describe()
  render(now)
end

function on_exit()
  badge.led.clear()
  badge.led.show()
end
```

### 6. NFC card viewer — on-demand NFC and last-seen card

Present a tag to the badge's reader to display its UID. **A** forgets the last
card and scans again; move the old tag away to scan a different one. **HOME**
exits. This reads the card identifier, not its NDEF text or private contents.

```lua
--[==[badge-app
slug=demo_nfc
name=NFC Card Viewer
icon=NFC
api=2
heap_kb=48
wake_lock=1
]==]

local enabled = false
local last_uid = nil
local next_poll = 0
local label, detail

function on_enter(root)
  local title = badge.ui.label(root, "NFC Card Viewer")
  title:align("top_mid", 0, 16)
  label = badge.ui.label(root, "Starting reader...")
  label:align("center", 0, -20)
  detail = badge.ui.label(root, "")
  detail:align("center", 0, 16)
  local hint = badge.ui.label(root, "A scan again   HOME exit")
  hint:align("bottom_mid", 0, -16)
  enabled = badge.nfc.enable()
  if enabled then badge.nfc.clear() end
  label:set_text(enabled and "Present an NFC tag" or "NFC unavailable")
end

function on_tick()
  if not enabled then return end
  local now = badge.sys.ms()
  if now < next_poll then return end
  next_poll = now + 200
  local card = badge.nfc.card()
  if card and card.uid ~= last_uid then
    last_uid = card.uid
    label:set_text("UID: " .. card.uid)
    detail:set_text(string.format("SAK %d   ATQA %d", card.sak, card.atqa))
  end
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED then return end
  if button == badge.input.BUTTON.A and enabled then
    badge.nfc.clear()
    last_uid = nil
    label:set_text("Present an NFC tag")
    detail:set_text("")
  end
end

function on_exit()
  if enabled then badge.nfc.disable() end
end
```

### 7. Nearby hello — two-badge radio, filtering, send cooldown

Install and open this app on **both badges**. **A** broadcasts a short hello;
the other badge shows the sender's radio address and signal strength. **HOME**
exits. A queued message is not a delivery acknowledgement; broadcasts may be
missed or repeated. This example does not send identity or contact data.

```lua
--[==[badge-app
slug=demo_radio
name=Nearby Hello
icon=HI
api=2
heap_kb=48
wake_lock=1
]==]

local enabled = false
local next_send = 0
local received = 0
local status, peer, signal

function on_enter(root)
  local title = badge.ui.label(root, "Nearby Hello")
  title:align("top_mid", 0, 12)
  status = badge.ui.label(root, "Starting radio...")
  status:align("top_mid", 0, 48)
  peer = badge.ui.label(root, "Open this app on both badges")
  peer:style({text_font = 14})
  peer:align("center", 0, 0)
  signal = badge.ui.label(root, "No messages yet")
  signal:style({text_font = 14})
  signal:align("center", 0, 32)
  local hint = badge.ui.label(root, "A send hello   HOME exit")
  hint:align("bottom_mid", 0, -16)
  enabled = badge.radio.enable()
  if not enabled then
    status:set_text("Radio unavailable")
    return
  end
  status:set_text("Ready - press A")
  badge.radio.on_recv(function(mac, rssi, payload)
    -- Filter the shared Lua channel to this app's exact message.
    if payload ~= "HELLO1:hi" then return end
    received = received + 1
    peer:set_text("From " .. mac)
    signal:set_text(string.format("%d dBm / %d frames", rssi, received))
  end)
end

function on_button(button, kind)
  if kind ~= badge.input.KIND.PRESSED or button ~= badge.input.BUTTON.A then return end
  if not enabled then return end
  local now = badge.sys.ms()
  if now < next_send then
    status:set_text("Wait a second, then A")
    return
  end
  next_send = now + 1000
  if badge.radio.send("HELLO1:hi") then
    status:set_text("Hello queued - A sends again")
  else
    status:set_text("Send failed - retry with A")
  end
end

function on_exit()
  if enabled then
    badge.radio.on_recv(nil)
    badge.radio.disable()
  end
end
```

## Help the user troubleshoot in the IDE

Keep troubleshooting focused on the IDE and the generated app. Start with
evidence already supplied; do not ask the user to repeat a traceback or code
they have given you. Distinguish import validation, USB transfer, launcher
discovery, and running the app. **`[push] reload confirmed` confirms the upload
and rescan, not successful app initialization.** Likewise, an `app_reg:
launched` line can follow a Lua startup error: inspect the preceding
`script_app` error and the badge screen.

### What to ask for and what to check

If information is missing, ask for the exact error text and traceback from
the IDE console, which action caused it (opening, moving, winning, resetting,
or exiting), and the complete single-file app they actually pushed. If a
firmware/API mismatch is plausible, obtain the version using the temporary
logging below. Do not require a repository checkout or terminal access.

Read the callback name first (`main.lua`, `on_enter`, `on_tick`, `on_recv`,
`on_button`, or `on_exit`), then follow the traceback through the entire call
path. The reported line is where an error was detected; for a deadline, it
is not necessarily the operation that used most of the time. Line numbers
refer to the IDE's extracted `main.lua`, **excluding the bundle header**.

When useful, tell the user to enter these read-only commands in the IDE
console, one at a time: `apps` lists installed apps, `heap` reports system and
LVGL memory, and `uitree` reports the current visible widgets and text.
`cat /littlefs/apps/THEIR_SLUG/main.lua` reads the installed code if there is
doubt about which version was pushed. Substitute the actual slug. Request
only output relevant to the problem; do not request a full device snapshot
or provisioning credentials.

For timing or allocation problems, add brief, temporary logs around the
suspected section inside the full app you return. For example, log once at
startup:

```lua
badge.sys.log("firmware=" .. badge.sys.version())
local s = badge.sys.stats()
badge.sys.log("lua=" .. s.lua_used .. "/" .. s.lua_limit ..
  " peak=" .. s.lua_peak .. " widgets=" .. s.widgets ..
  " free_heap=" .. s.free_heap)
```

Use `badge.sys.ms()` before and after a bounded section to measure elapsed
milliseconds. A timeout prevents the final log from running, so place sparse
milestone logs between phases if necessary. Logging itself takes time; never
print per cell or every tick. Remove or rate-limit diagnostic logging in the
final app. Host syntax checks and mocked API tests do not reproduce ESP32
timing, LVGL allocation, flash-write latency, or USB behaviour.

### Match the fix to the failure

| Symptom | What to fix or tell the user |
| --- | --- |
| Import button is absent | Save the draft first, then refresh the IDE. If still absent, copy only the header's `key=value` lines into `manifest.cfg`, and all code after `]==]` into `main.lua`. Do not keep directing the user to a button they cannot see. |
| Import reports a bad header/config | Include exactly one complete app. Keep the delimiter lines intact. Use `api=2`, `heap_kb=48` or `96`, and valid `key=value` lines. Import does not fix Lua syntax. |
| No device in the picker / port already in use | Use desktop Chrome or Edge on the HTTPS IDE, a data cable, and a direct USB port. Close other IDE tabs or serial tools, turn the badge off then on with USB attached, and reconnect. A browser agent may need the user to select the device in the browser's permission picker. |
| Connected, but waiting for `badge> ` | Unplug and restart normally without holding Start, then reconnect. If the launcher or console is still unavailable, ask the user for the exact IDE status and what appears on the badge. |
| Upload timeout | Read the IDE status: retry if it recovered; otherwise restart the badge, reconnect and Push the complete app again. A partial upload is not an installed working app. |
| Push succeeds but no launcher entry | Read console errors and send `apps`. Check slug/name, filename spelling, folder/slug match, valid manifest, and built-in slug collisions. Try `reload`, then Reboot. Avoid `lua_countdown`: that bundled directory is currently explicitly skipped. |
| New config options seem ignored | Reboot after changing runtime manifest options on an existing slug. |
| Lua syntax error / missing `end` / unexpected symbol | Inspect the extracted `main.lua`, including the lines before the reported line. Check matching blocks, quotes, and that prose or Markdown fences were not pasted into the editor. Import success does not compile Lua. Return the complete corrected app. |
| `attempt to call a nil value` | Check the exact function and `:` versus `.`. Use `badge.ui.label`, not `badge.label` with API 2. Remove invented APIs and unavailable standard libraries, including `pcall`, `xpcall`, `os`, and `coroutine`. If a documented API is absent, investigate the installed firmware version. |
| `root widget is read-only` | Create a child box/label under root and style that object. |
| Widget/type/style error | Check the exact method, supported properties/fonts, relative installed `.bin` image paths, and integer coordinates. Boxes and buttons need a child label for text; `button:set_text()` is not supported. Floating-point sensor math must be rounded before use as coordinates. |
| `widget has been deleted` | A deleted parent also deletes its children. Clear retained handles and pending animation/reset work; do not use a child handle after deleting its parent. Prefer reusing the existing UI. |
| `Lua execution deadline exceeded` in `on_enter` | Reduce cumulative startup work: remove duplicate painting, set static styles once, build large UIs incrementally. Updated firmware allows 3,000 ms; older versions allow 250 or 1,000 ms. Neither `heap_kb=96` nor fewer than 512 widgets proves it will fit. |
| Deadline in `on_button` | Updated firmware allows 1,000 ms; older versions allow only 20 ms. Look for whole-board resets, win highlighting, search, repeated storage access, and synchronous hardware work. Update state/changed cells and schedule bounded remaining work across ticks. |
| Deadline in `on_tick` or `on_recv` | The shared cutoff is 250 ms on updated firmware, versus 6 ms previously, including queued radio handlers. A few ms remains the smooth-animation target. Limit work per invocation, redraw only changes, cap catch-up loops, throttle messages/logging, and queue expensive reactions. Moving a whole startup loop into one tick makes the allowance smaller. |
| Deadline in the main chunk or `on_exit` | Updated firmware allows 3,000 ms for top-level execution and 1,000 ms for exit; older firmware allows 500/100 ms. Move UI setup into lifecycle callbacks and keep shutdown/save work small. Do not perform a whole export or large filesystem rewrite during exit. |
| `Lua memory limit exceeded`, including `used < limit` | The current message covers both quota rejection and system `realloc` failure. `heap_kb` reserves no RAM; the failed allocation is absent from `used`/`peak`, and GC/cleanup may already have lowered usage. Check `main.lua` versus a lifecycle callback and the pre-launch `heap` output. A `main.lua` failure can be compilation memory pressure: reduce the full app's code/state complexity rather than only spreading widgets across ticks. Reboot once to check retained memory/fragmentation; never promise 96 KiB is available or recommend deleting apps as a RAM fix. See the memory explanation above. |
| `too many widgets (512)` / failed widget allocation | Reuse widgets or delete a no-longer-needed parent and clear its handles. Losing a Lua reference does not delete a native widget. Inspect `heap` and repeated-round behaviour; increasing Lua heap does not fix native allocation pressure. |
| `Lua stack safety limit reached` | Remove deep recursion, recursive search, and long callback/helper chains. Use an explicit bounded work queue. This is separate from the Lua heap quota and execution deadline. |
| Error card / app stops ticking | Fix the cause first. Three consecutive tick failures suspend ticks; one button failure also suspends them. A retries from existing state, while HOME and reopening start fresh. A cannot revive a Lua state closed by a startup failure. Do not keep retrying a broken loop. |
| One press acts twice / controls do nothing | Handle only `badge.input.KIND.PRESSED` for one-shot actions. Use numeric `badge.input.BUTTON` constants, not strings. UI `clickable` does not add Lua event handlers; handle physical buttons in `on_button`. Ignore game actions until incremental setup finishes. |
| Timer jumps after HOME confirmation | `confirm_home=1` pauses ticks, not `badge.sys.ms()`. There is no cancel callback. Use default HOME exit, or explicitly design pause/resume and cap elapsed-time catch-up work. |
| Scores disappear / saving causes stutter | Reopening creates a fresh Lua state. Save only changed values; respect store key/string quotas and signed integer limits. Explain when saves occur. Exit-only saving requires a normal exit; power loss/startup failure does not guarantee a save. Avoid flash access every frame. |
| LEDs stay dark, illuminate the wrong side, or stop animating | Call `show()` after staging the whole frame; use integer RGB channels from 0–255 and Lua indices 1–6. Follow the front-view map, not C++ zero-based indices or the wearer's left/right. Low logical brightness may appear off. Effects need foreground ticks and pause during HOME confirmation; system sync can temporarily override them. Use the LED Tour example to identify individual positions. |
| Image upload is unsupported | Help the user save/remove the optional local `icon.bin` and use the manifest text icon for a text-only app upload. If needed, have them report the exact IDE error to badge support. |
| Old image/file remains after deletion | Local deletion does not delete remote files. Remove the exact unwanted remote path, then `reload`. |
| NFC/radio does nothing | Check enable results and unavailable-hardware errors. For radio, both apps must be open, in range, and agree on a payload prefix; queued does not mean delivered. Keep packets within 44 bytes and tolerate duplicates/loss. NFC text reads are hardware work; do not poll them every frame. |
| Reboot, panic, or blank screen instead of a Lua error | Ask for the boot/panic log and triggering action, then reduce to a small reproducer. A process restart or native allocation failure may require firmware investigation; do not label every failure as a Lua timeout or assume defective hardware. Preserve the user's app and data. |

When replying with a fix, tell the user what failed and why, which work you
changed, what you verified, and the remaining device-test limits. Return the
**entire app with the same slug** and the installation steps matching their
IDE. Ask them to try the exact failing action again, plus startup, repeated
reset, a win/draw if applicable, and exit/reopen. Do not say an app is safe
from timeouts just because it uses valid API calls or only 42 widgets. Do not
recommend erasing the badge, deleting unrelated files, or treating an ordinary
Lua error as a hardware failure.
