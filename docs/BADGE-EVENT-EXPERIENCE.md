# The Hacker Badge Event Experience

**Canonical spec — Hack the North 2026.** Supersedes the original Notion draft.

This is the single source of truth for the end-to-end badge experience: what an attendee sees, what the firmware does, what the backend must provide, and what logistics must execute. It is written against the **as-built firmware**, so every "already done" claim cites a file.

| | |
|---|---|
| Hardware | ESP32-C3, 4 MB flash, 320×240 ST7789, 6× WS2812, MFRC522 NFC, SC7A20H accel, 9 buttons |
| Firmware | ESP-IDF 5.5.3, LVGL 9 via `esp_lvgl_port`, LittleFS, USB-Serial-JTAG console |
| Repo root | This repository is the IDF project root |
| Last updated | 2026-08-01 |

---

## 1. Scope, glossary, status legend

### Status legend

Every section carries a status. Read these literally — they were determined by reading the code, not by asking.

| Tag | Meaning |
|---|---|
| `BUILT` | Works today. Cited file path is the implementation. |
| `PARTIAL` | Substantial code exists but does not yet meet this spec. |
| `NOT STARTED` | No code exists. |
| `BLOCKED` | Cannot be built as originally described; this spec gives the replacement. |

### Glossary

| Term | Meaning |
|---|---|
| **badge_id** | 8-byte opaque, server-generated, non-sequential identifier. The only identifier safe to broadcast or print. |
| **attendee_id** | HTN-internal numeric ID. Never leaves the badge over the air. |
| **claim_id** | Extended profile claim ID, used by the dashboard for live networking lookups. |
| **slug** | Stable string ID for an app (`scanner`, `blindbox`). Doubles as the NFC unlock token. |
| **launcher_slugs** | The ordered list of installed apps. Membership = installed, index = grid position. This *is* the unlock set. |
| **Default app** | Always visible, cannot be dropped or moved. `App::access() == AppAccess::Default`. |
| **BBX1** | The badge's BLE wire protocol. 18-byte header + ≤232-byte body, rides manufacturer data on company ID `0xFFFF`. |
| **relay** | A mains-powered ESP32-C3 that bridges BBX1 ↔ HTTPS. Badges have no Wi-Fi. |
| **beacon box** | A relay in ceremonies mode: takes cues over WebSocket, rebroadcasts them over BLE. |
| **upload station** | An NFC tag + a relay. Tap to sync your badge to HAPI. |

### What this document does not cover

Physical design, lanyards, the judge sticker (a print job), booklet copy, and the Solana business relationship. Those are tracked elsewhere.

---

## 2. Hardware baseline & constraints

**Status: `BUILT`** — [`main/pins.h`](../main/pins.h), [`partitions.csv`](../partitions.csv), [`sdkconfig.defaults`](../sdkconfig.defaults)

### Pin map — all 12 GPIOs in use

| Pin | Use | Pin | Use |
|---|---|---|---|
| GPIO0 | LCD DC | GPIO7 | 74HC165 DATA |
| GPIO1 | LCD CLK | GPIO9 | Start button (direct, active-low) |
| GPIO2 | LCD CS | GPIO10 | LCD MOSI |
| GPIO3 | WS2812 DIN | GPIO20 | 74HC165 LOAD |
| GPIO4 | LCD RST | GPIO21 | 74HC165 CLK |
| GPIO5 | I2C SDA | | |
| GPIO6 | I2C SCL | | |

Eight buttons (`A, B, Home, Down, Left, Right, Up, Aux1`) arrive over the 74HC165 shift register; `Start` is a dedicated GPIO. Order is defined in [`main/hal/hal_buttons.h`](../main/hal/hal_buttons.h).

### Constraints that shape the whole design

These are not preferences. They are properties of the board and they killed several ideas in the original draft.

1. **No battery sense.** No ADC pin, no divider, no fuel gauge on the I2C bus. **The badge cannot know or display its battery level, and cannot warn on low battery.** Any UX that depends on battery percentage is out.

2. **No backlight control.** No backlight pin exists. The LCD backlight draws current whenever the power switch is on, and firmware cannot dim or disable it. The panel *contents* could be blanked via the ST7789 display-off command, but the light stays on — so we never bother (§13).

3. **RTC-capable GPIOs are 0–5, and all six are consumed** by LCD/LED/I2C. Deep sleep can therefore only be woken by timer, never by a button. **This is why we use light sleep** — see §13.

4. **GPIO9 is the ESP32-C3 boot-mode strapping pin.** Holding Start while the chip resets can drop it into serial download mode: blank screen, unresponsive, looks bricked. Recovery is a power cycle. This matters at registration (§5) and it is a reason to avoid deep sleep, whose timer wake re-runs the ROM bootloader.

5. **No Wi-Fi, no TCP/IP, no HTTP in the badge binary.** The root `CMakeLists.txt` sets `MINIMAL_BUILD ON` and `main/CMakeLists.txt` does not list `esp_wifi`, `esp_netif`, or `esp_http_client`. All internet access is proxied via BLE relay (§11).

6. **The shared I2C bus** carries the accelerometer (`0x19`) and NFC (`0x26`). Do not add a second bus.

### Flash budget

```
nvs       data nvs       0x9000   0x4000     16 KB    (NimBLE only)
phy_init  data phy       0xd000   0x1000      4 KB
factory   app  factory   0x10000  0x2A0000  2.625 MB  (~1.45 MB used, ~1.1 MB free)
storage   data littlefs   —       0x140000  1.25 MB   (identity lives here — §3)
                                  ends at 0x3F0000
                          ---- 64 KB free to 0x400000, unallocated ----
```

**No partition table change is required.** Identity is a file in `storage` (§3). The 64 KB tail is left free; note it exists if we ever need a partition that must survive a filesystem format.

### RNG entropy — a hard precondition

`esp_random()` is **only a true RNG when Wi-Fi or Bluetooth is enabled** (ESP-IDF `components/esp_hw_support/include/esp_random.h:20-21`). The radio is enable-on-demand and **off at boot**.

> **Any code that generates a key, nonce, or identifier MUST call `hal_radio_enable()` first and check it succeeded.** Generating the Solana keypair (§14) at first boot with the radio down would draw weak entropy on 1000 badges at the same point in their boot sequence, risking correlated or duplicate keys.

---

## 3. Badge identity model

**Status: `NOT STARTED`** — nothing in the firmware today stores a badge ID, role, attendee ID, or networking fields.

What exists is `/littlefs/config/card.cfg` with free-text `name`/`email`/`url`/`tag`, written only over serial ([`hal_console.cpp:184`](../main/hal/hal_console.cpp)) and read by [`app_card.cpp`](../main/apps/app_card.cpp) and [`app_badge_bump.cpp`](../main/apps/app_badge_bump.cpp).

### Storage: a JSON file in LittleFS

**`/littlefs/identity.json` is the canonical store.** It is written directly by the registration tool over serial and parsed at boot with cJSON (already a dependency).

Deliberately a plain file, not `hal_config` and not NVS: the registration tool writes exactly the bytes it means to write, `cat /littlefs/identity.json` works for debugging, apps and Lua read it without linking NVS, and there is no format conversion step between the wire and the disk.

> **Accepted risks.** Three ways a badge can lose its identity, all knowingly accepted:
> - `hal_fs_init()` sets `format_if_mount_failed = true` ([`hal_fs.cpp:17`](../main/hal/hal_fs.cpp)) — filesystem corruption reformats and anonymises the badge.
> - A full-erase flash wipes it. The web flasher uses `eraseAll: false` ([`tools/web-flasher/index.html:220`](../tools/web-flasher/index.html)) and writes only `0x0`/`0x8000`/`0x10000`, so normal re-flash is safe — but **"Erase Flash" destroys identity and the Solana keypair.** The flasher must never expose that button.
> - `factory_reset` does `rm -rf /littlefs/config` ([`hal_console.cpp:303`](../main/hal/hal_console.cpp)). `identity.json` sits at the LittleFS root, **not** under `config/`, so it survives — keep it that way, and add a test, because the natural "make factory_reset more thorough" edit would break identity fleet-wide.
>
> Mitigation for all three is the same: re-provision at the badge desk, or restore over BLE using `mac → badge_id` (§5, §11).

### The record

New module `main/hal/hal_identity.{h,cpp}`. The on-disk form is JSON; this is the in-memory shape apps read.

```c
enum BadgeRole : uint8_t {
    ROLE_HACKER = 0, ROLE_ORGANIZER, ROLE_SPONSOR, ROLE_JUDGE,
    ROLE_MENTOR, ROLE_VOLUNTEER, ROLE_MEDIA, ROLE_STAFF,
};

typedef struct {
    uint16_t version;               // 1
    bool     provisioned;

    char     badge_id[64];          // opaque, server-generated, non-sequential
    uint32_t attendee_id;           // HTN internal — never broadcast
    uint8_t  role;                  // BadgeRole
    uint8_t  role_color[3];         // RGB; drives LEDs + UI accent, matches lanyard
    uint32_t provisioned_unix;

    char     display_name[40];      // first + last, not necessarily legal name
    char     account_email[64];     // account email, for tracking. NOT broadcast.
    char     claim_id[24];          // extended profile claim ID

    char     net_email[64];         // networking fields — these ARE shared on bump
    char     net_phone[20];
    char     net_linkedin[40];
    char     net_discord[40];
    char     net_instagram[32];
    char     net_x[32];
} badge_identity_t;
```

`badge_id` is a **64-byte string field**, so the server is free to use hex, base58, a UUID, or a prefixed form without a firmware change. Everything downstream (bump packets, upload payloads, QR codes) treats it as an opaque string.

The Solana keypair (§14) is a separate file, `/littlefs/solana.json`, on the same terms.

### API

```c
esp_err_t hal_identity_init();                  // parse identity.json, populate the struct
bool      hal_identity_is_provisioned();
const badge_identity_t *hal_identity_get();     // nullptr if unprovisioned
esp_err_t hal_identity_reload();                // re-read after provisioning, no reboot
void      hal_identity_erase();                 // staff only — unlink identity.json
```

`hal_identity_init()` runs early in `app_main`, after `hal_fs_init()` and before app registration.

If `identity.json` is missing, malformed, or fails validation, the badge is **unprovisioned** — it boots to the dead-end screen in §6. Never half-load a record; a badge showing someone else's name is worse than one showing none.

### badge_id must be opaque

Generate it **server-side** from a CSPRNG at registration. Never on the badge, never derived from the MAC.

The draft correctly worried about sequential IDs being scrapable. Deriving from the MAC is equally bad — MACs are near-sequential across a production reel, so a scraper who sees a handful can enumerate the rest.

> **Keep it short in practice.** The field allows 64 bytes, but `badge_id` is carried in every bump packet (§10), which is airtime-constrained. **16–22 characters is the sweet spot** (128 bits as hex or base58). Using all 64 costs ~42 bytes in every single bump exchange.

> **Residual risk, accepted:** the BLE static random address ([`hal_radio.cpp:64`](../main/hal/hal_radio.cpp), derived from the Wi-Fi MAC with the top two bits forced) is stable across reboots and broadcast whenever the radio is on. With 1000 badges that is a location-tracking vector regardless of `badge_id`. We need it stable for bump dedupe and transfer session leases, so we accept it — but **nothing that maps a MAC to a person may ever be broadcast.**

---

## 4. RSVP → data contract

**Status: `NOT STARTED`** (backend work)

Everyone except volunteers sees this step. Framing for the RSVP copy: *we're asking for the name you'd like displayed on your badge, and the networking info you want people to get when they scan it.*

### New extended-profile-claim fields

| Field | Type | Max | Notes |
|---|---|---|---|
| `badge_display_name` | string | **39 chars** | Doesn't have to be legal name; must have a first and last name. **New field.** |
| `net_email` | string | 63 | Existing networking field |
| `net_phone` | string | 19 | Existing |
| `net_linkedin` | string | 39 | Store the vanity slug, not the full URL — the badge prepends `linkedin.com/in/` |
| `net_discord` | string | 39 | Existing |
| `net_instagram` | string | 31 | Handle without `@` |
| `net_x` | string | 31 | Handle without `@` |

### Validation is the backend's job, not the badge's

Every max above is **one less than the C buffer** in `badge_identity_v1_t`, leaving room for the NUL. The badge truncates silently if given more, which produces a mangled name on someone's badge that nobody notices until the event.

**The registration tool must reject over-length values before flashing, not truncate them.** Enforce the same limits at RSVP time so the problem surfaces while the attendee can still fix it.

Also enforce at RSVP: `badge_display_name` contains at least two whitespace-separated tokens, and is **printable ASCII plus the Latin-1 supplement** (U+0020–U+007E and U+00A0–U+00FF). That covers accented Latin names — José, Müller, Ångström — which is the decided scope.

> **Firmware task:** the built-in `lv_font_montserrat_*` fonts cover ASCII only by default. **They must be regenerated with the Latin-1 supplement range** or accented characters render as blanks or boxes. This is a build-config change plus a flash-size check, not a code change, but it must happen before the first badge is provisioned with a real name.
>
> CJK and other scripts are **out of scope**. The only CJK font in the build is a ~40-glyph subset generated for the factory-test strings ([`tools/image-convert/gen-cjk-font.sh`](../tools/image-convert/gen-cjk-font.sh)) — a full CJK face is ~1.4 MB and does not fit. **The RSVP form must reject out-of-range characters with a clear message**, not silently accept a name that will render blank on the badge.

---

## 5. Registration

**Status: `PARTIAL`** — the serial transport works ([`hal_console.cpp:113`](../main/hal/hal_console.cpp) `put`), the provisioning protocol on top of it does not exist.

See also: Badge Registration Flow (internal planning doc).

### Who gets a badge

| Full badge (flashed with identity) | Basic badge (no flash) |
|---|---|
| Organizers + VLs + Kari | Volunteers |
| Hackers | General |
| Sponsors | Visitor |
| Judges (+ physical JUDGE sticker) | |

Non-badge groups still register and still get a **virtual badge ID** in HAPI so goose games and dashboard features work.

### Station setup

Each registration computer has a scanner gun and a USB-C cable. Two volunteers per station:

- **Stocking volunteer** — fetches badges from stock, plugs the next one in, hands out programmed badges and batteries.
- **Registration volunteer** — at the computer, scans tickets and clicks.

### The provisioning protocol

Reuses the tested `put` upload path rather than inventing new framing. **The JSON is written straight to its final location** — no staging file, no commit-and-rename step:

```
badge> put /littlefs/identity.json 512
READY
<512 raw bytes of JSON>
OK 512
badge> prov apply
PROV OK id=7f3a9c11d4e02b58 mac=c8:2e:18:04:9a:71 name=Ada Lovelace role=hacker
```

`prov apply` re-reads `identity.json`, parses it with cJSON (already a dependency — `json` is in `PRIV_REQUIRES`), validates **every** field length, calls `hal_identity_reload()`, **resets the onboarding flag**, and drives the operator receipt below. On failure it prints `PROV FAIL <reason>` on one line and changes nothing.

The onboarding reset (`badge_onboarding_reset()`) matters: a badge that just got an owner is about to be handed to someone who has never used one, so it must replay the tutorial on first boot regardless of how many times it was powered up during testing or a previous registration.

> Because the write is direct, a failure between `put` and `prov apply` leaves a badge holding an unvalidated `identity.json`. `hal_identity_init()` treats a malformed or over-length record as **unprovisioned** (§3), so the badge boots to the dead-end screen rather than showing garbage. The operator sees a red LED and re-does the badge. This is the accepted tradeoff for skipping staging.

Supporting commands:

| Command | Purpose |
|---|---|
| `prov show` | Print identity.json + BLE MAC, machine-parseable |
| `prov mac` | BLE static random address only |
| `prov erase confirm` | Staff only. Separate from `factory_reset` so the two can never be confused. |

> **Record the BLE MAC at provisioning time.** The `PROV OK` line includes it, and the registration tool **must** store `mac → badge_id` in the DB. This single line of foresight is what makes field restore possible (§11): a badge that loses its identity can be re-identified over BLE by MAC alone, with the attendee needing to remember nothing. Reading the MAC requires NimBLE to be up (~300 ms), which `prov apply` does once.

### Operator feedback — two channels

The operator is holding a stack of badges, not watching a serial log.

- **The LCD is the receipt.** `prov apply` renders a full-screen confirmation: large display name, role-coloured band, `badge_id` in mono, green check or red X. A photo of the screen is a complete audit record.
- **The LEDs are the glanceable state.** Amber sweep while writing → **three green flashes then solid role colour for 10 s** = OK. **Solid red** = fail. The badge visually becomes *yours* at the moment of provisioning, which is a nice product beat as well as an operational signal.

### Volunteer runbook (print this)

1. **The badge must be switched OFF before plugging in.**
2. Plug the badge into the USB-C cable.
3. Click **Connect** in the registration app. Approve the macOS/Chrome serial port popup if it appears.
4. *(If needed, this is when a full re-flash happens.)*
5. **— HACKER ARRIVES HERE —** Scan their ticket with the gun. Confirm ID.
6. Choose **Flash to badge** or **Register without badge**.
7. Click **Register**. Wait for the badge screen to show their name and the LEDs to flash green.
8. Unplug and hand over the badge **plus batteries** — tell them to insert carefully, right way around.
9. Stocking volunteer plugs in the next badge. Return to step 5.

> ⚠️ **Do not hold the Start button while plugging in or powering on.** GPIO9 is the boot-mode strapping pin; holding it at reset can put the badge into serial download mode — blank screen, looks broken. Fix: unplug, release all buttons, power cycle.

> ⚠️ **Never use "Erase Flash."** It destroys the identity partition and the attendee's Solana keys.

### Re-flash and restore, day-of

- Firmware re-flash preserves identity automatically (`eraseAll: false`).
- `factory_reset` must **preserve `/littlefs/identity.json` and `/littlefs/solana.json`** while clearing the tutorial flag. It only removes `/littlefs/config` and `/littlefs/apps` today, and both files sit at the LittleFS root, so it is already safe — **add a comment and a test**, because the natural "make it more thorough" edit would break identity fleet-wide.
- Full restore from a badge backup: see §16.

---

## 6. Startup, first boot, tutorial, waiver

**Status: `NOT STARTED`**

### Boot splash

On every startup, show the **Hack the North logo and the Solana logo on the same screen for one second**, then continue.

If the badge is unprovisioned (`!hal_identity_is_provisioned()`), show instead:

> **This badge hasn't been set up yet.**
> Please bring it to a registration computer.

with a **Restore my badge** option (§11) and a staff override on a held button combo.

### Onboarding as a swappable shell

Do not add a mode to the launcher. Instead swap the *shell* app, which reuses the registry's existing behaviour: [`app_registry.cpp`](../main/apps/app_registry.cpp) already traps the global HOME button when `s_active == s_launcher`, so making onboarding the shell gives a modal flow for free.

```c
// app_registry.h — new
void app_registry_set_launcher(App *launcher);
```

```c
// main.cpp, after HAL init, before app_registry_start()
App *shell = app_badge_id_instance();          // normal boot target — see §7
if (hal_config_get_int("system", "onboard_build", 0) < BADGE_BUILD_NUMBER) {
    shell = app_onboarding_instance();
}
app_registry_start(shell);
```

On completion, onboarding writes `system.onboard_build = BADGE_BUILD_NUMBER` and calls `app_registry_set_launcher(app_badge_id_instance())`, then returns to it.

### The flow

1. **Welcome.** "Press any button to continue."
2. **Identity confirm.** Name and role from NVS, role colour on the LEDs.
3. **Button tutorial.** Explain each button; **require pressing all of them** to advance. Uses the existing `ButtonEvent` dispatch.
4. **Hardware demos.**
   - LED demo.
   - NFC demo — tell them to look for the NFC icon around the event and open the scanner app when they find one.
   - Accelerometer mini-demo (a quick tilt/shake experience).
5. **Sleep explanation.** *After 5 minutes idle your badge sleeps to save power. The screen will tell you to press START to wake it. Flipping the switch off and on works too.*
6. **What's on the badge.** Get them excited about the apps and what's unlockable.
7. **Rules + agreement.** Show the badge rules (same as the back of the badge), point to [badge.hackthenorth.com](https://badge.hackthenorth.com), require ticking a checkbox. `Down` must reach the bottom of the scroll before Accept enables.
8. **Accept** → `hal_config_set_int("system", "onboard_build", BADGE_BUILD_NUMBER)`.

### Completion state

One value, in LittleFS: `system.onboard_build`. **Provisioning resets it** (§5), so every registered badge shows the tutorial once, on the first boot after it is handed over. It stores the build number of the firmware the user completed onboarding on. There is no separate waiver flag — **completing onboarding is the acceptance record**, since the checkbox is the last step.

`BADGE_BUILD_NUMBER` is an integer baked in at compile time and **incremented on every build** (a `BADGE_BUILD` env var consumed in [`main/CMakeLists.txt`](../main/CMakeLists.txt), same pattern as the existing `BADGE_FACTORY_TEST` flag). Display it in the info app so a support volunteer can read a badge's exact build off the screen.

> **Consequence to be aware of:** because the stored value is compared against the compiled-in build number, **any firmware re-flash replays onboarding.** That is the desired behaviour during development. If we re-flash badges at the event for a bug fix, every affected attendee re-does the tutorial — This is **accepted** — we will not freeze the build number for hotfixes. Volunteers doing a day-of re-flash should warn the attendee they will see the tutorial again.

At the next upload station, `onboard_build` syncs to HAPI as confirmation that setup completed.

---

## 7. Badge ID page & home screen

**Status: `PARTIAL`** — [`app_card.cpp`](../main/apps/app_card.cpp) has a QR view, but it encodes the bare name string ([`app_card.cpp:68`](../main/apps/app_card.cpp)) and knows nothing about roles, badge IDs, or pals.

### A normal app, auto-launched on boot

`main/apps/app_badge_id.cpp`, slug `badgeid`. No launcher surgery, no pane axis — it is an ordinary `App` subclass that happens to be the boot target.

- **Registered first** in [`main/main.cpp`](../main/main.cpp), so it is also the first tile in the launcher grid.
- `access()` returns `AppAccess::Default` — always visible, cannot be dropped or moved.
- **Launched at boot** instead of the launcher: `app_registry_start(app_badge_id_instance())` (or `app_onboarding_instance()` on first boot, §6).
- **HOME returns to the launcher** via the existing global HOME-button handling in [`app_registry.cpp`](../main/apps/app_registry.cpp) — no new code. From the launcher, the ID page is the first tile.

This needs one small registry addition, `app_registry_set_launcher(App *)`, so onboarding can hand off (§6). Everything else is existing behaviour.

While in here: the launcher's active page-dot recolour was never implemented — [`app_launcher.cpp:252`](../main/apps/app_launcher.cpp) `(void)`-casts `COLOR_DOT_ACTIVE` to silence the unused-variable warning. Worth fixing.

### ID page content

- **Full display name**, large.
- **QR code** encoding `https://hackthenorth.com/b/<claim_id>` — *not* the bare name as today.
- **Role**, as a coloured band using `role_color`, consistent with lanyards.
- **Space for a pal.**

**Two states.** Before the scavenger hunt: plain background. After unlocking characters (§14): the chosen **hacker pal renders as the page background**, equippable from the Solana app.

### App framework addition

One new virtual on [`app.h`](../main/apps/app.h), needed by §13:

```c
virtual bool wants_wake_lock() const { return false; }  // block sleep while active
```

---

## 8. Default apps

**Status: mixed** — see per-row status.

These ship installed (the `AppAccess::Default` tier) on every badge.

| App | Status | Notes |
|---|---|---|
| **Badge ID** | `PARTIAL` | New `app_badge_id.cpp`, §7. **Registered first**, auto-launched on boot. |
| **Completion / progress** | `NOT STARTED` | New `app_progress.cpp`. See below. |
| **Welcome booklet** | `NOT STARTED` | TL;DR of event info + QR to the full booklet. |
| **Networking** | `PARTIAL` | [`app_contacts.cpp`](../main/apps/app_contacts.cpp) lists bumped contacts and works. Needs `badge_id` keying and the new networking fields. |
| **A simple game** | `BUILT` | Snake ([`app_snake.cpp`](../main/apps/app_snake.cpp)) — persists a high score, and goose games already wants "beat snake." Ship Snake as the default; the rest stay unlockable. |
| **Solana Scavenger Hunt** | `PARTIAL` | [`app_blindbox.cpp`](../main/apps/app_blindbox.cpp), registered immediately after My Badge. See §14. |
| **Sponsor map** | `NOT STARTED` | See §15. |
| **Info** | `NOT STARTED` | QR to [badge.hackthenorth.com](https://badge.hackthenorth.com) + the dashboard, including the schedule. |
| **Ticket** | `NOT STARTED` | Trivial — redirects to the ID page app. |
| **Mailbox (sync)** | `NOT STARTED` | Explains the upload-station system; triggers a sync. See §11. |
| **Scanner** | `BUILT` | [`app_scanner.cpp`](../main/apps/app_scanner.cpp) — the single central NFC app. Already Default tier. |

### The progress app

`main/apps/app_progress.cpp`, slug `progress`.

**All totals are hardcoded at compile time.** No server-fed category config, no `progress.cfg` — the denominators are known when we build the firmware, and hardcoding them removes the entire sync-down dependency for this app.

```c
static constexpr Category kCategories[] = {
    { "Apps",        count_unlockable_apps() },  // derived from the registry
    { "Sponsors",    18 },                       // set to the real booth count
    { "Solana Scavenger Hunt",  5 },                       // 5, possibly up to 9 — §14
};
```

**Every count is derived locally**, so the ring moves the instant something happens rather than waiting for a sync:

| Category | Numerator source |
|---|---|
| Apps | Unlocked launcher entries whose access tier is `Public` |
| Sponsors | Distinct `table_number`s in `/littlefs/config/sponsors.cfg` (§15) |
| Solana Scavenger Hunt | `all_items` count from the Solana app's stored state (§14) |

**Contacts is a count, not a fraction.** There are ~2500 people at the event — a denominator would be absurd and a bit grim. Show *"You've met 23 people"* with no bar.

Render: an `lv_arc` percentage ring for overall completion, a large `17/42`, per-category rows with mini bars, and the contacts count as a plain line underneath.

Set the sponsor count once the booth list is final (§15) and note it in the build. Getting it wrong shows everyone a percentage that can never reach 100%.

---

## 9. App unlocking

**Status: `BUILT`** — [`app_scanner.cpp`](../main/apps/app_scanner.cpp), [`launcher_slugs.cpp`](../main/apps/launcher_slugs.cpp)

This already works end to end and needs only content, not code.

### How it actually works

Apps declare one of three access tiers in [`app.h`](../main/apps/app.h):
`Default`, attendee-facing `Public`, or staff-only `Internal`. `access()` is
the only visibility control: Default apps bypass the launcher slug list
entirely, while unlocked Public and Internal apps appear through membership in
`launcher_slugs`.

- Tap an NFC tag whose **NDEF text record is exactly an app slug**.
- [`app_scanner.cpp`](../main/apps/app_scanner.cpp) accepts registered Public apps and calls `launcher_slugs_append()`. It rejects Internal targets.
- The slug list is persisted as a CSV in `/littlefs/config/system.cfg` under `launcher_order`. **Membership = installed, index = grid position.**
- Unlock triggers a celebration: icon zoom-lerp plus six Lissajous orbit motes ([`app_scanner.cpp:205`](../main/apps/app_scanner.cpp)).
- Re-scanning an already-unlocked slug shows an "already got it" state.
- Registration can seed Default, Public, or Public + Diagnostics modes through the permanent `appmode` console command. Internal apps are never unlocked by NFC.

### Constraints

- **The CSV is capped at 1024 bytes** ([`launcher_slugs.cpp:9`](../main/apps/launcher_slugs.cpp)) and entries past the cap are **silently dropped on write**. At ~10 chars per slug that is ~90 apps — fine, but if we ever approach it the failure is silent. Add a log line at 80% capacity.
- Only **NDEF Well-Known Text** records are parsed (TNF=0x01, type `'T'`) — [`hal_nfc.cpp:218`](../main/hal/hal_nfc.cpp). URI records will not work.
- Only the **first record** of the message is read, and only the **first 64 bytes** of tag memory (pages 4/8/12/16).

### Tag → slug placement

| Location | Slug | Unlocks |
|---|---|---|
| Organizer HQ | `credits` | Organizer credits app |
| Sponsor bay | `linkedin` | Big LinkedIn QR app |
| After all sponsor booths | *(server-pushed)* | Bonus game |
| Various | `blindbox`, games, tools | See the unlock list below |

Print the full unlock list in the badge site so people know what they're hunting. Use the same sticker art as the sponsor taps.

---

## 10. Networking Badge Bump

**Status: `PARTIAL` — and it has a live privacy bug.**

[`app_badge_bump.cpp`](../main/apps/app_badge_bump.cpp) implements mutual-bump contact exchange and works: both badges must detect an accelerometer jerk within ~500 ms (`kSyncWindowTicks = 25`), checked in both directions, with an EMA baseline, a tunable threshold, and an 8-tick refractory.

### The privacy bug — fix this first

`hal_radio_send()` does **not** transmit a discrete message. It **overwrites a persistent BLE advertisement** that then repeats every ~200 ms until something else overwrites it or the radio is torn down ([`hal_radio.cpp:280`](../main/hal/hal_radio.cpp)).

`app_badge_bump.cpp:154` writes the full `BumpPacket` — name, email, url, tag — into that advertisement on the first bump. **It then keeps broadcasting your contact details, in the clear, every 200 ms, for as long as the app is open.** Anyone in the room with a phone BLE scanner harvests everyone who has ever bumped.

This directly contradicts the product requirement *"I don't want to share my info with people I didn't bump."*

**Fix (decided):** transmit only in a **~200 ms window** after a detected bump, then stop advertising.

This needs an API that doesn't exist yet but is cheap. `hal_radio_disable()` blocks up to 2 s because of `nimble_port_deinit`, but `ble_gap_ext_adv_stop()` on its own is a non-blocking HCI command:

```c
// hal_radio.h — new
esp_err_t hal_radio_adv_pause();     // ble_gap_ext_adv_stop(kAdvInstance)
esp_err_t hal_radio_adv_resume();    // ble_gap_ext_adv_start(kAdvInstance, 0, 0)
esp_err_t hal_radio_set_adv_interval(uint32_t min, uint32_t max);
esp_err_t hal_radio_set_scan_duty(uint16_t itvl, uint16_t window);
```

### ⚠️ 200 ms does not work at the default radio timings

This is the single most likely way the bump feature ships broken, so it is called out rather than buried.

The default advertising interval is **200–220 ms** ([`hal_radio.cpp:29`](../main/hal/hal_radio.cpp)) and the default scan is **20% duty** — 30 ms window per 150 ms interval ([`hal_radio.cpp:34`](../main/hal/hal_radio.cpp)). A 200 ms transmit window at those settings emits **roughly one advertising event**, and the peer has a ~20% chance its scan window is even open when that event lands. Most bumps would silently fail.

**While the bump app is open, both radios must be reconfigured:**

| Setting | Default | During Badge Bump |
|---|---|---|
| Adv interval | 200–220 ms | **30 ms** (`set_adv_interval`) |
| Scan duty | 20% (30/150 ms) | **100%** (`set_scan_duty(160, 160)`) |

At 30 ms, a 200 ms window emits ~6–7 advertising events across all three primary channels, and a continuously-scanning peer will catch them. The cost is irrelevant — this only applies while the app is foregrounded.

### Both badges scan continuously; only the transmit is windowed

The two bumps are not simultaneous — the existing mutual-detection logic already tolerates ±500 ms (`kSyncWindowTicks = 25`). If both badges only transmitted *and* listened for 200 ms, the windows could miss each other entirely.

They don't have to overlap. The receive path stays always-on:

1. The badge **scans continuously** for the whole time the app is open, buffering recently-heard packets with timestamps.
2. On its own bump, it starts transmitting.
3. Pairing succeeds if a peer packet was heard **within ±500 ms of its own bump** — the existing peer-table logic, unchanged.

So a badge can receive its partner's card before, during, or after its own bump. Only the *outgoing* PII is windowed, which is the entire point.

### Stop transmitting on acknowledgement, not on a timer

A fixed window forces a bad trade: too short and the peer misses it, too long and PII sits on the air after it was already delivered. **Both problems disappear if the packet carries an acknowledgement.**

Add one more TLV field: **`ack_id`** — the `badge_id` of a peer heard within the last 500 ms, if any.

```
A bumps  ─┬─ transmits card (ack_id absent)
          │
B bumps  ─┼─ transmits card, ack_id = A     ← B heard A already
          │
A sees its own badge_id echoed in B's ack_id
          └─ A stops advertising immediately: delivery is confirmed
```

Rules:

- **Stop advertising as soon as your own `badge_id` appears in a peer's `ack_id`.** Delivery is proven; there is no reason to keep broadcasting.
- **Hard cap 400 ms**, then stop regardless. Covers the case where the peer never transmits (walked away, app closed, one-sided bump).
- Emit `ack_id` for the most recent peer heard inside the sync window. It costs ~18–24 bytes.

| | Fixed 200 ms | Ack-gated |
|---|---|---|
| Typical PII exposure | 200 ms always | **~60–150 ms** |
| Peer missed the frame | Silent failure | Keeps retrying to 400 ms |
| Worst case | 200 ms | 400 ms |

This is **both more private and more reliable** than a fixed window: typical exposure roughly halves, and a badge that hasn't been heard keeps trying instead of giving up at an arbitrary deadline. The 400 ms cap only applies when nobody is listening, which is exactly the case where a longer transmit is harmless.

> **Residual risk, explicitly accepted:** a scraper standing in a crowd during bump windows still collects PII. The alternative considered was exchanging only `badge_id` and resolving contacts server-side, which leaks nothing — rejected because there is no server fetch available and it would make the contacts app useless offline. **Say this out loud in the privacy copy** rather than implying bumps are private.

### The v2 bump packet — all fields

There is no server-side contact resolution, so **everything the recipient needs must be in the packet.**

```
magic[4] = "BMP2"
version  = 2
role     = 1 byte
then TLV records, only for non-empty fields:
  [tag:1][len:1][utf8 bytes]
  tags: badge_id, display_name, net_email, net_phone,
        net_linkedin, net_discord, net_instagram, net_x,
        ack_id   <- badge_id of a peer heard in the last 500 ms
```

**Variable-length TLV, not a fixed struct.** A fixed struct with every field at max width is 338 bytes; most attendees fill three or four networking fields, so TLV typically lands at **~110–160 bytes** — inside the current 250-byte cap, with better range because the frame is shorter.

| Case | Size | Fits 250 B cap? |
|---|---|---|
| Typical (name, email, LinkedIn, Discord) | ~110–160 B | Yes |
| All eight fields at maximum length | ~347 B | **No — needs the raised cap** |

The worst case depends on the chained extended-advertising work (open question E, now decided: yes). Until that lands, **truncate at the frame cap in a defined field order** — `badge_id`, `display_name`, `net_email` first, so the fields that matter most always make it. Never emit a partial TLV record.

This is also why `badge_id` should stay short in practice (§3): it is in every packet.

`contacts_store_save()` keys contact files on `badge_id` rather than MAC. Contacts still upload as IDs plus timestamps (§11), keeping the upload payload small — the full card is stored locally for offline display.

**Remove the hardcoded personal defaults.** [`app_badge_bump.cpp:228`](../main/apps/app_badge_bump.cpp) `load_owner_card()` falls back to a developer's own name, email, and domain. **Any badge with an unwritten card broadcasts these.** Replace with identity from `hal_identity_get()`, and broadcast nothing at all if unprovisioned.

**Persist the bump threshold.** Currently tunable at runtime but lost on exit.

**Use RSSI to kill false positives.** The receive callback now carries `int8_t rssi` ([`hal_radio.h:34`](../main/hal/hal_radio.h)) and badge bump ignores it. Gate acceptance on RSSI above a threshold so a bump detected coincidentally across the room can't pair. `app_blindbox.cpp:474` already has the pattern, including the **`rssi == 127` "unavailable" sentinel guard** — reuse it.

### Tuning targets

Priority order, from the product brief:

1. **No false positives.** Never exchange with someone you didn't bump.
2. **Few false negatives.** A deliberate bump should work.

**Set the jerk threshold to 500 mg** (down from the current default of 1200 mg), keeping the 200–3000 clamp. A gentle, deliberate tap should register — a firm bump should not be required. The precision then comes from the other two levers, not from making people hit each other hard.

Levers: the jerk threshold (**500 mg**), the sync window (currently 500 ms), and the new RSSI gate. Lowering the threshold will raise the raw detection rate *and* the false-positive rate — RSSI gating plus mutual detection is what pulls the false positives back out. Expect to tune the RSSI gate harder than you otherwise would because of this.

### IRL test protocol

This needs extensive real-world testing, starting **early September when Nick is in Waterloo**. Minimum:

| Test | Pass criteria |
|---|---|
| 2 badges, deliberate bump, 50 reps | ≥95% success |
| 2 badges, 1 m apart, one shaken hard, 50 reps | 0 false pairings |
| 10 badges in a bag, shaken | 0 pairings |
| 20 badges in a crowd, 10 deliberate bumps | ≥90% success, 0 false pairings |
| Bump while 30 badges advertise nearby | No regression vs quiet room |

Record RSSI distributions during these runs and pick the gate from data, not a guess.

---

## 11. Upload stations & BBX1 transfer

**Status: `BUILT`, not yet run on hardware.**

> **What shipped, and where it departs from the design below.** The badge side is
> [`fs_snapshot`](../main/apps/fs_snapshot.cpp), [`bbx_xfer`](../main/apps/bbx_xfer.cpp)
> and [`app_sync`](../main/apps/app_sync.cpp); the station is
> [`tools/sync_station/`](../tools/sync_station/). Upload only — the downlink
> in "Downlink, restore, and sync-down" is not implemented, and `want_downlink`
> is always 0.
>
> Five deliberate departures:
>
> 1. **The payload is the whole filesystem, not the curated blob.** Every text
>    file under `/littlefs` goes up with its contents; binaries go up as stubs
>    carrying path, size and reason, so the server can tell a deleted file from
>    an omitted one. ~6–20 KB rather than the ~1.3 KB budgeted below.
> 2. **`bbx_xfer_stat_t.bitmap` is 32 bytes, not 16.** 16 covers 128 frames =
>    29,696 B, which is 1.5x headroom over the payload — and overrunning it
>    corrupts bits silently rather than erroring. `seq`/`total` stay `uint8_t`,
>    so the ceiling is 255 frames = 59,160 B; widening those is a
>    header-breaking v2.
> 3. **A station beacons `BBX_TYPE_SYNC_BEACON`, not `BBX_TYPE_BEACON`.**
>    `app_blindbox` accepts any BEACON as a Solana Scavenger Hunt id, so sharing the type
>    would make a badge in proximity mode fire a REQ at a sync desk.
> 4. **`bbx_xfer` is driven from the Sync app's `on_tick()`, not the registry
>    tick.** `hal_radio` holds a single recv callback, so a registry-driven
>    transport would have to own that slot permanently and multiplex it. The
>    "composes with any app's `on_tick()`" premise below does not survive the
>    HAL.
> 5. **Advertising is not paused during `AwaitStat`.** The held
>    `OPEN|SWEEP_DONE` is what re-asks for a STAT; pausing leaves a lost STAT
>    with no self-healing path. Instead, a settling window makes STATs fresh —
>    see the trap in [REMAINING-WORK.md](REMAINING-WORK.md) §5.
>
> Simulated badge-against-station over a channel modelling the register
> semantics: 6.3 s clean, still completing at 50% frame loss in 8.4 s with 3
> repair rounds. Nothing has run on real silicon.

> The draft says *"we open a quick wifi connection."* **The badge has no Wi-Fi, no TCP/IP, and no HTTP stack**, and adding them costs ~400–600 KB of flash, SSID/PSK provisioning at registration, Wi-Fi/BLE coexistence on the C3's single radio, and a real battery hit.
>
> **Decision: reuse the BLE relay.** [`tools/relay_firmware/`](../tools/relay_firmware/) is a working ESP32-C3 BLE↔HTTPS bridge with a versioned protocol, LRU dedupe, ACKs and status codes. An upload station is **an NFC tag plus a relay box.**

### Locations

Help desk - goose games desk - badge desk - **every Solana scavenger hunt tap** (so hunt taps double as sync points).

### What gets uploaded

A single JSON blob, stored server-side as the badge's restorable backup:

- Networking info and contacts
- Solana scavenger hunt state **including the private key** (accepted — see §14)
- Sponsor booth taps
- App unlock state, progress counters, any other app metadata

**Make `badge_id` the contact primary key on the wire.** A `BumpPacket` is 173 bytes; 50 contacts as full cards is ~8.6 KB, which is slow and overflows the frame bitmap. Uploading `{badge_id, unix}` per contact is ~14 bytes — the server already knows who that is. Combined with a `last_sync_seq` high-water mark for deltas:

| Section | Bytes |
|---|---|
| Envelope + identity delta + waiver flag | ~200 |
| 60 contacts as `{id, unix}` | ~840 |
| Solana Scavenger Hunt / achievement flags | ~120 |
| Counters, app usage, progress | ~150 |
| **Total** | **~1.3 KB** |

### Protocol changes — the 18-byte header does not change

`seq` and `total` already exist in [`bbx_proto.h`](../main/apps/bbx_proto.h), reserved for fragmentation and always 0/1. `_rsv` is free for a session ID. All existing senders write zeros, so this is backward compatible.

```c
    uint8_t  seq;        // frame index, 0..total-1
    uint8_t  total;      // frame count; 1 = unfragmented (rev 1 compatible)
    uint8_t  status;     // BBX_ST_*
    uint8_t  sess;       // WAS _rsv. 0 = no session.

#define BBX_TYPE_XFER_OPEN   0x10
#define BBX_TYPE_XFER_GRANT  0x11
#define BBX_TYPE_DATA        0x12
#define BBX_TYPE_XFER_STAT   0x13   // receiver -> sender, bitmap of what landed
#define BBX_TYPE_XFER_DONE   0x14
#define BBX_TYPE_CEREMONY    0x20   // §12
#define BBX_TYPE_IDENT_REQ   0x30   // field restore
```

```c
typedef struct __attribute__((packed)) {
    uint8_t  kind;          // UPLOAD / RESTORE / SYNC_DOWN / IDENT
    uint8_t  enc;           // 0 = raw JSON
    uint16_t total_len;
    uint32_t crc32;
    uint16_t chunk_len;
    uint8_t  total_frames;
    uint8_t  want_downlink;
} bbx_xfer_open_t;          // 12 bytes

typedef struct __attribute__((packed)) {
    uint8_t  total;
    uint8_t  recv_count;
    uint8_t  bitmap[16];    // bit i = frame i received; covers 128 frames
} bbx_xfer_stat_t;          // 18 bytes
```

> ⚠️ **Both copies of `bbx_proto.h` must change together** — [`main/apps/bbx_proto.h`](../main/apps/bbx_proto.h) and [`tools/relay_firmware/main/bbx_proto.h`](../tools/relay_firmware/main/bbx_proto.h). The relay is a separate IDF project and cannot include from `main/`. Editing one and not the other **fails silently as "the badge never gets a response."**

### The crux: a persistent advertisement is a register, not a queue

`hal_radio_send()` writes a value that stays visible until overwritten. Two rules turn that into a reliable protocol:

1. **Every payload must be held for a dwell period** long enough that at least one advertising event carries it and the peer's scan window catches it.
2. **The peer's own advertisement is the acknowledgement channel.** Both sides poll each other's register. This is an alternating-state protocol, not request/response.

New module `main/apps/bbx_xfer.{h,cpp}` — a transport state machine, not an app, driven from the 50 Hz registry tick so it composes with any app's `on_tick()`:

```c
enum class XferState { Idle, Opening, Sweeping, AwaitStat, Repairing, Done, Failed };

void        bbx_xfer_start_upload(const XferConfig &cfg);
void        bbx_xfer_tick();
void        bbx_xfer_on_frame(const uint8_t mac[6], int8_t rssi, const uint8_t *d, size_t n);
XferState   bbx_xfer_state();
int         bbx_xfer_progress_pct();
const uint8_t *bbx_xfer_downlink(size_t *len_out);
```

Sequence — **selective repeat with a bitmap**, not stop-and-wait, because a per-frame round trip is where all the time goes:

```
badge   OPEN(kind, len, crc, chunk_len, N)   held 400ms, repeated until GRANT
relay   GRANT(sess=7, chunk_len=232)         held until first DATA or 3s
badge   DATA(sess=7, seq=0) dwell            <- one sweep, no per-frame RTT
        DATA(sess=7, seq=1) dwell
        ... seq=N-1
badge   OPEN(flags=SWEEP_DONE)               held 600ms; badge pauses adv, listens
relay   STAT(sess=7, bitmap)                 held 800ms
badge   DATA for each clear bit              repair sweep
        ... until bitmap full
relay   DONE(sess=7, status, want_downlink)
```

`hal_radio_adv_pause()` during `AwaitStat` is what stops the badge's own advertisement polluting the channel while it listens.

### Throughput

| Configuration | Adv interval | Payload/frame | Throughput | 1.3 KB dump |
|---|---|---|---|---|
| As-built | 200 ms | 232 B | 0.77 KB/s | ~2 s + repair |
| `set_adv_interval(30ms)` | 30 ms | 232 B | 3.9 KB/s | ~0.4 s |
| 30 ms + chained ext adv | 30 ms | ~1580 B | 15 KB/s | 1 frame |

**Dropping the advertising interval to 30 ms during a transfer is the single highest-leverage change here.** Minimum legal non-connectable ext-adv interval is 20 ms; 30 ms is safe. Power cost is irrelevant for a 5-second transaction.

Also raise the badge's scan to 100% duty during `AwaitStat`. At the default 20% duty ([`hal_radio.cpp:34`](../main/hal/hal_radio.cpp)) the expected time to catch a relay advertisement is ~1 s, which would dominate every round trip.

**The 250-byte frame cap is self-imposed in software.** Both badge and relay already build with `CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE=1650`; the limit lives in `hal_radio_send()` ([`hal_radio.cpp:283`](../main/hal/hal_radio.cpp)) and `ble_link_set_payload()`, plus their 260-byte stack buffers. If chained `AUX_CHAIN_IND` works at station range, a full dump becomes one frame. **Bench-test this in week 1** — it changes the station UX from "hold still" to "instant."

### Relay-side changes

> ⚠️ **The dedupe LRU will silently eat every upload fragment.** `seen_before_locked()` ([`tools/relay_firmware/main/main.c:127`](../tools/relay_firmware/main/main.c)) keys on `(badge, req_id)`, and every DATA frame in a transfer shares one `req_id` — so frames 1..N drop as duplicates. This presents as *"uploads always stall after the first frame"* and will cost a day if not pre-empted. **Gate dedupe to `BBX_TYPE_REQ` only**; DATA frames are idempotently deduped by the receive bitmap.

- Replace the fixed `job_t.body` with a heap reassembly session (`badge[6]`, `sess`, `buf`, `len`, `crc`, `bitmap[16]`, `expires_us`). One active session, 16 KB max — fine against ~300 KB free heap with Wi-Fi + BLE up.
- **One adv instance means one session at a time** (`MAX_EXT_ADV_INSTANCES=1` on both sides). `XFER_GRANT` is a lease: the relay grants to one MAC, embeds the grantee's MAC in every subsequent frame, and denies others with `BBX_ST_BUSY` plus a `backoff_ds`. Badges seeing a GRANT for another MAC show *"Station busy — 2 ahead"* and retry with jitter. **Without the lease, two badges at one station corrupt each other's sessions.**
- Raise the payload cap and stack buffer to 1650/1660 behind a Kconfig so the big-MTU experiment is one flag.

### Downlink, restore, and sync-down

Same machinery with roles swapped, piggybacked on the same session. `XFER_DONE` carries `want_downlink`; the relay then sends its own `XFER_OPEN` on the same `sess` and the badge grants. **Never overlap sweeps** — there is one adv slot per side.

The badge writes the received blob to `/littlefs/config/sync_down.json`, and a `sync_apply()` pass fans it out to identity (restore), `progress.cfg`, and `launcher_order` (server-pushed unlocks).

**Field restore** uses `BBX_TYPE_IDENT_REQ`: an unidentified badge broadcasts it (the header's `badge[6]` is its MAC), the relay POSTs it, the server looks up `mac → badge_id` from registration (§5) and returns the full identity blob over the downlink. This is why recording the MAC at provisioning time matters.

If possible, sync-down also refreshes the attendee's **networking info** from their live claim, so a badge picks up RSVP edits made after registration.

### Backend requirements

- A new HAPI resolver accepting the badge JSON blob, service-token authenticated, authorization-guarded.
- Stored per attendee, keyed by `badge_id`, retrievable read-only by the owner from the dashboard frontend.
- Upload triggers goose games quest evaluation (§16).

---

## 12. Ceremonies remote control

**Status: `BLOCKED` as originally written → `NOT STARTED` with the replacement design.**

> The draft has volunteers running **"beacon web apps" on their phones** that broadcast to badges. **This is not possible.** Web Bluetooth cannot advertise — browsers can only act as a BLE *central* (scan and connect), never as a broadcaster. No browser on any platform can do this.
>
> **Decision: ~12 dedicated ESP32-C3 beacon boxes**, driven from an admin web app over WebSocket.

```
admin control app (auth required)
        │  WebSocket
        ▼
  [beacon box] × ~12 around Laz
        │  BLE ext-adv, continuous
        ▼
     badges
```

### Beacon boxes are a build mode of the relay, not a new project

[`tools/relay_firmware/`](../tools/relay_firmware/) already has `wifi_sta.c`, `ble_link.c`, and a working build. Add `CONFIG_RELAY_MODE_BLINDBOX` vs `CONFIG_RELAY_MODE_CEREMONY` plus a `ws_client.c` on `esp_websocket_client`. Ceremony mode replaces the worker/ticker tasks with a WS receive loop and a 200 ms rebroadcast ticker. **One binary, one flashing procedure, one BLE stack to debug.**

Coverage: roughly 3 at the top of Laz, ~9 in the main area — **confirm during rehearsal**, that's what rehearsal is for.

### The frame

```c
typedef struct __attribute__((packed)) {
    uint8_t  proto;         // 1
    uint8_t  effect_id;     // LED program
    uint8_t  screen_id;     // preloaded full-screen graphic
    uint8_t  cflags;        // bit0 WAKE_SLEEPERS, bit1 FORCE_SCREEN_ON, bit2 SHOW_END
    uint32_t show_id;       // random per show; a change is a hard reset
    uint32_t t_ms;          // ms since show start, at the instant of this frame
    uint16_t seq;           // freshness
    uint8_t  color[3];      // primary RGB
    uint8_t  color2[3];     // secondary RGB
    uint16_t period_ms;
    uint8_t  brightness;
    uint8_t  beacon_idx;    // which box sent this — diagnostics
    uint8_t  pw_hash[8];    // first 8 bytes of SHA-256(password)
} bbx_ceremony_v1_t;        // 32 bytes → 50-byte frame
```

A 46-byte frame fits in a **single primary-channel advertising PDU** — no chaining, maximum reliability, minimum airtime. Graphics are preloaded on the badge and referenced by `screen_id`; colours and timing are sent live, so effects stay flexible.

### Timing across ~12 beacons

**Do not build a distributed clock.** Two layered mechanisms:

- **Cue model.** The admin app sends `{"type":"cue","show_id":…,"effect":7,"color":[255,0,128]}` over WebSocket. Each beacon starts its local `t_ms` at 0 on receipt. Delivery spread across 12 boxes on one AP is ~10–50 ms — invisible for an LED show. Ship this.
- **`t_ms` in every frame**, so a badge waking mid-show lands at the right point in the animation rather than restarting it.

Badge side: on the first frame, `show_epoch_ms = now_ms() - t_ms`, then run the animation off the badge's own `esp_timer`. On later frames compute `drift`; if `|drift| < 150 ms` ignore it, otherwise **slew** `show_epoch_ms` by ~5 ms per tick toward the new value.

> **Never snap.** With 12 beacons at slightly different offsets, snapping produces a visible stutter every time a badge hears a different box. Slewing makes drift invisible. Hard-reset only when `show_id` changes.

### Activation and polling

- Badges poll for a ceremonies beacon **every 3 minutes**, riding through light sleep (§13).
- **Activate remote control 5 minutes before you need effects** (3 min poll + 2 min margin). The control app shows a counter of how long remote control has been active.
- A badge **polls immediately on power-on**, so someone switching on mid-effect joins in.
- Beacons **continuously rebroadcast** the current state — the advertisement *is* the show state, refreshed on every WS message and on a 200 ms ticker. Late joiners catch up within one scan window.
- Badges entering ceremony mode run 100% scan duty for the first 2 s to lock on fast, then drop back to 20%.
- **Show end:** the `SHOW_END` flag, or no ceremony frame for 5 seconds → exit and restore the previous app.

### Effects required

- Basic selectable colours.
- The **end-of-ceremonies flashing sequence** for the story beat, with a coordinated screen graphic.
- A **Solana button** — their gradient on the LEDs and their logo on screen.

Used twice in opening ceremonies (Solana's on-stage demo; the final fade appearance) and once in closing (the colour-returns celebration). Coordinate the story beats with the ceremonies scripting and story teams.

### Security posture — stated honestly

The beacon sends a **shared password** in `pw_hash`; the badge compares it against a **hardcoded SHA-256 prefix** baked into the firmware. The password itself never appears in the binary — only its hash — so dumping a badge's flash does not directly reveal it.

```c
// badge side
static const uint8_t kCeremonyPwHash[8] = { /* first 8 bytes of SHA-256(password) */ };
bool ceremony_auth_ok(const bbx_ceremony_v1_t *f) {
    return memcmp(f->pw_hash, kCeremonyPwHash, 8) == 0;
}
```

The badge needs no hashing at all at runtime — the beacon transmits the precomputed digest prefix and the badge does a `memcmp`. mbedTLS SHA-256 is available if we'd rather transmit the password and hash on-badge, but there's no benefit; either way the value on the air is replayable.

**Known and accepted limitations:**

- **Replay.** Anyone who captures one valid frame can rebroadcast it forever. Accepted.
- **Once the password is observed on the air, it's public.** The hash protects the firmware image, not the channel.
- 8 bytes is ample against brute force here — the attack that matters is capture-and-replay, which no amount of hashing prevents.

This is the correct amount of security for a lightshow, and it's cheap. Just don't describe the channel as authenticated — the worst case is someone flashes LEDs at the wrong moment, and we can pull power on the beacons.

### Operational requirements for the admin app

Admin login required to control; anyone may run a beacon. Needs: WebSocket reconnect with backoff (12 boxes on conference Wi-Fi *will* drop), a per-box last-seen heartbeat in the UI, and a large **ALL STOP** that sends `SHOW_END`. **Twelve boxes on congested conference Wi-Fi is the operational risk here, not the protocol.**

Ceremonies also needs a slide telling hackers to turn their badges on, noting it's fine if the screen goes dark.

---

## 13. Sleep

**Status: `NOT STARTED`** — there is no sleep, no idle detection, and no screen timeout anywhere in the firmware today.

### Light sleep, not deep sleep

Deep-sleep GPIO wake on the C3 requires RTC-capable pins (GPIO 0–5), and all six are consumed (§2) — so deep sleep could only ever be woken by a timer, never a button. **Light sleep has no such restriction.**

The power argument agrees with the UX argument: deep sleep saves ~200 µA against a floor of ~4–6 mA from the six WS2812s' quiescent draw plus an uncontrollable LCD backlight. It was buying nothing measurable while costing button wake.

Light sleep also removes the two most fragile pieces of the alternative: no `RTC_DATA_ATTR` state block, and no headless deep-sleep boot branch in `app_main`. And because it doesn't re-run the ROM bootloader, **the GPIO9 strapping hazard disappears** for wake (it still applies at plug-in, §5).

### Tier state machine

`main/hal/hal_sleep.{h,cpp}`, driven off `s_last_input_ms` stamped in the registry's button dispatcher:

| Tier | Entry | What happens | Exit |
|---|---|---|---|
| `ACTIVE` | input | 50 Hz tick, screen on | 60 s idle |
| `DIM` | 60 s idle | LVGL tick → 10 Hz, LEDs off | any input |
| `LIGHT` | **5 min idle** | Draw the sleep card, then `esp_light_sleep_start()` in a loop; wake on **Start** or the 3 min ceremonies timer | Start → `ACTIVE`; timer → poll, then back to `LIGHT` |

```c
void hal_sleep_init();
void hal_sleep_note_activity();
void hal_sleep_tick();
void hal_sleep_wake_lock(bool on);
SleepTier hal_sleep_tier();
```

`App::wants_wake_lock()` (§7) blocks anything past `DIM`. Override `true` in: the games, `app_lightshow`, `app_pomodoro`, `app_flashlight`, `app_blindbox`.

### Wake source: Start only

`gpio_wakeup_enable(GPIO_NUM_9, GPIO_INTR_LOW_LEVEL)` — GPIO9 is a direct active-low pushbutton, no hardware change, no bench test needed.

The other eight buttons are behind the 74HC165 shift register on GPIO7 and are **not** wake sources. That is fine, because the screen tells the user what to press.

### The sleep card — do not blank the screen

The LCD backlight cannot be turned off (§2), and the ST7789 holds its framebuffer through light sleep without the ESP32 driving it. **So blanking the display saves nothing and communicates nothing.** Instead, immediately before `esp_light_sleep_start()`, draw a full-screen card and leave it up:

```
        [ HTN logo ]

         Sleeping

    Press START to wake
```

It stays legible for the entire sleep, costs zero power versus a blank screen, and removes the whole class of "my badge is broken" support questions. This is strictly better than the display-off approach and is the reason no `SCREEN_OFF` tier exists.

Add the badge's display name in small text at the bottom, so a badge left on a table can still be returned to its owner.

### Required notes

- **No `hal_display_set_on()` is needed** — we never turn the panel off. (The panel handle is file-local in [`hal_display.cpp:37`](../main/hal/hal_display.cpp); leave it that way.)
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE` is currently **not set**, so the automatic light-sleep path is off despite `CONFIG_PM_ENABLE=y`. We call `esp_light_sleep_start()` explicitly, so this is fine — but note `esp_pm_configure()` is never called either, so DFS runs on auto-init defaults only.
- On wake, restore the pre-sleep screen before the first frame renders, or the user sees the sleep card flash.

### Ceremonies poll through sleep

Because LVGL, LittleFS and NimBLE all stay resident, a poll wake costs ~50 ms of setup plus the listen window — roughly 10× cheaper than the deep-sleep equivalent. Keep NimBLE resident and only pause advertising and scanning; do not tear the stack down (`hal_radio_disable()` blocks up to 2 s and must never be called from a tick path — see the comment at [`app_blindbox.cpp:786`](../main/apps/app_blindbox.cpp) explaining a crash it caused).

Every 3 minutes: wake → `ceremony_listen(3000)` **leaving the sleep card on screen** → if a show is live, enter ceremony mode; otherwise re-enter light sleep. That is ~1.7% radio duty.

### User-facing behaviour

The badge shows *"Press START to wake"* and responds to Start. Flipping the power switch off and on also works. Onboarding (§6) teaches this, and the on-screen text means nobody has to remember it.

**We cannot show a battery level or warn on low battery — there is no battery sense hardware.**

---

## 14. Solana scavenger hunt

**Status: `PARTIAL`** — [`app_blindbox.cpp`](../main/apps/app_blindbox.cpp) (886 lines) is a complete, working app. The crypto and the real backend contract are missing.

Background: Solana Scavenger Hunt Idea (internal planning doc).

### What already works

- **One trigger**: RSSI-gated proximity to a relay beacon — walk up and it fires. No modes, no naming.
- The proximity trigger is genuinely well-built: Q4 fixed-point RSSI EMA, up to 4 tracked boxes, ≥3 samples required, 1500 ms staleness window, tunable threshold (default −60 dBm, clamped −90..−30, persisted), and re-arm requiring **both** a 5 s cooldown and the signal dropping 10 dB below threshold.
- Full `Idle → Opening → Reveal → Failed` state machine with a 12 s timeout and a gold border pulse.
- Requests go over BBX1 to the relay; responses are parsed with cJSON.
- A green "relay" indicator distinguishes *"no relay in range"* from *"relay heard me but the request failed."*

### What's missing

**1. The backend contract doesn't match.** Solana are building the server side. The agreed contract:

```jsonc
// POST <RELAY_WEBHOOK_URL>  -  Content-Type: application/json
{
  "box":        "blind-box-01",
  "user_id":    "123456",
  "name":       "Ada Lovelace",
  "public_key": "5oNDL3swdJJF1g9DzJiZ4ynHXgszjAEpUkxVYejchzrY",
  "email":      "ada@example.com"
}
```

```jsonc
// 200 OK
{
  "new_item":   "1",
  "chain_link": "https://explorer.solana.com/...",
  "all_items":  ["1", "3", "5"]   // source of truth, includes the new one
}
```

Done — the badge sends `{box, user_id, name, public_key, email}` and reads
`{new_item, chain_link, all_items}` ([`app_blindbox.cpp`](../main/apps/app_blindbox.cpp)
`build_request()` / `finish_open()`). Field notes for reference:

| Field | Shape |
|---|---|
| `user_id` | `attendee_id` as a string, not the BLE MAC. |
| `public_key` | Base58 ed25519 public key (libsodium derivation, `solana_wallet`). |
| `email` / `name` | `account_email` / `display_name` from identity (§3). |
| `new_item` | String id; empty renders "Empty box?". |
| `chain_link` | Stored per reveal; drives the B-button QR (quest console or explorer). |
| `all_items` | Authoritative inventory — **replace local state with this**, don't merge. |

**`all_items` is the source of truth.** If a response is lost, the next box resyncs the full inventory, which makes the whole flow self-healing over a lossy BLE link. Store it wholesale.

**2. Crypto is done** (`solana_wallet.cpp`): libsodium ed25519 keypair, hand-rolled base58, generated on first app entry with the radio up for entropy, persisted at `/littlefs/solana.json`. The badge never signs — the server mints to the public key.

### Item → icon mapping

There are **5 rewards, possibly up to 9**. Item IDs are **strings** in the JSON (`"1"`, `"3"`) — keep them strings on the badge; do not parse to int, or a future non-numeric ID breaks the mapping silently.

The badge holds a compile-time table from item ID to pal icon (e.g. `"2"` → Patch, our mascot). **Build the table with all 9 slots from day one**, with unknown IDs rendering a generic placeholder rather than crashing or showing nothing — Solana may add rewards after we freeze the firmware.

### Crypto implementation

Keys are **self-managed on-device**. Not production — no secure element, no attestation, and the private key is exportable at upload (the draft already assumes this: *"solana scavenger hunt info (including privatekey lol)"*).

> **Decided: the badge never signs transactions.** The server mints to the badge's public key and returns `chain_link`. This means we need **public-key derivation only** — roughly half the work, and no signing code at all.

Two constraints, verified against the local IDF 5.5.3 install:

- **mbedTLS ships Curve25519 but not Ed25519** (`esp_config.h:490` has only `MBEDTLS_ECP_DP_CURVE25519_ENABLED`). The bundled TLS stack **cannot** produce Solana keypairs. libsodium is not in-tree either — it's a component-registry dependency (`espressif/libsodium`).
- **`esp_random()` is only a true RNG when Wi-Fi or Bluetooth is enabled** (§2).

| Piece | Approach | Why |
|---|---|---|
| Ed25519 pubkey derivation | **TweetNaCl** — one public-domain file. Use `crypto_sign_keypair` and simply ignore `crypto_sign`; the linker drops what isn't called. | Hand-rolling field arithmetic is ~1500 lines where mistakes are **silent** and still produce valid-looking keys. Smaller than pulling all of libsodium. |
| base58 | **Hand-rolled**, ~40 lines | Trivial, no security surface. |
| Entropy | `esp_fill_random()` **after** `hal_radio_enable()` succeeds | Otherwise 1000 badges draw weak entropy at the same point in their first boot. |

> **Hard precondition.** Keygen must call `hal_radio_enable()` first and **assert it succeeded**. Do not generate a keypair on a boot where the radio failed to come up — defer instead.

Flash cost is a few tens of KB against ~1.1 MB free in `factory`.

### Key lifecycle and state

Generated once on first entry to the Solana app — **not at boot**, because the radio must be up for entropy. Stored at `/littlefs/solana.json`, which sits at the LittleFS root so `factory_reset` does not remove it (§3):

```json
{
  "public_key":  "5oNDL3swdJJF1g9DzJiZ4ynHXgszjAEpUkxVYejchzrY",
  "secret_key":  "<base58 64-byte seed+pubkey>",
  "all_items":   ["1", "3", "5"],
  "chain_links": { "1": "https://explorer.solana.com/...", "3": "..." },
  "equipped":    "3"
}
```

Exported wholesale in the upload blob (§11) so a lost badge can be restored with its wallet and inventory intact.

### The hunt

1. The Solana app explains the hunt and sends you to the **Solana booth to start**. Tapping a non-first box before starting tells you to go there.
2. Each box opened unlocks a **pal customization** via the blockchain (`new_item`).
3. Pals are **equippable from inside the Solana app** and render as the **background of the ID page** (§7).
4. **5 boxes, possibly up to 9**, around the event.
5. Each item stores its `chain_link`; tapping it shows a **QR to that transaction** on the Solana explorer, so attendees can see the token transfer to their own wallet on-chain.
6. **Every hunt tap doubles as an upload station** (§11).

---

## 15. Sponsor tables

**Status: `NOT STARTED`** — see Visit Sponsor Booth Badge Tap (internal planning doc).

### Tag contents

Each sponsor table has an NFC tag under the standard sticker, carrying **only**:

```
sponsor_name   (slug)
table_number
```

Nice names, logos, and links live **on the badge**, not on the tag — so a typo doesn't require re-programming tags at the event.

> ⚠️ **The NDEF record must be Well-Known Text (TNF=0x01, type `'T'`)** and fit in the **first 64 bytes** of tag memory. [`hal_nfc.cpp:218`](../main/hal/hal_nfc.cpp) parses only the first record of a text NDEF message, and reads only pages 4/8/12/16. URI records will not work. Encode as e.g. `spon:ramp:12`.

### Tag programming and locking — needs a decision

> **`hal_nfc` is read-only. There is no NDEF writer, no `rc522_nxp_write`, and no key auth in the firmware.** Tags must be programmed by an external tool.

Options, in order of preference: a phone app (NXP TagWriter or similar) for programming plus static lock bits; or a small desktop NFC writer. **Locking requires setting the tag's static lock bytes or a password — decide before ordering tags**, and confirm the chosen tag type (NTAG213/215/216) supports what we need. This is a procurement question with a lead time, not a firmware question.

### Flow

1. Hacker opens the **scanner app** — the single central NFC app, so nobody has to know which kind of tap they're doing.
2. It reads the tag and records the sponsor + table number.
3. It **redirects to the sponsor map app**.
4. The map shows sponsor table locations, lit up by `table_number`.
5. Clicking a table shows that sponsor's detail — nice name, logo, links.
6. A **completion percentage** sits in the top bar.
7. Filling the map unlocks a **bonus game**.

### Map design

Approximately realistic to the real sponsor bay layout. **We are on two floors this year** — the design must handle a floor toggle or a stacked view. Coordinate with design early; this is the only app in the spec whose layout depends on a physical floorplan that may still be changing.

---

## 16. Dashboard, backup/restore, admin tooling, goose games, website

### 16.1 Dashboard ↔ badge communication (web serial)

**Status: `PARTIAL`** — [`tools/badgectl/`](../tools/badgectl/) is a working Monaco + WebSerial Lua IDE that already drives the console (`mkdir` / `put` / `reboot` / Ctrl-C, waiting on the `badge> ` prompt). [`tools/web-flasher/`](../tools/web-flasher/) does esptool-js firmware flashing.

What's needed is a **shared WebSerial library** extracted from `badgectl/web/app.js`, used by four consumers: the registration app (§5), the upload/sync page, the backup/restore page, and the admin tool.

> ⚠️ **There is no badge→host download command.** `put` uploads only, and `cat` is not binary-safe — it appends a trailing `\n` ([`hal_console.cpp:88`](../main/hal/hal_console.cpp)) and has no length framing. **Add a `get <path>` command** that prints `LEN <n>` then exactly `n` raw bytes. Everything in this section depends on it.

### 16.2 Backup and restore

- **Upload:** the same JSON blob as §11, stored in HAPI as the badge's restorable backup. Reachable over BLE at a station *or* over USB from the dashboard ("Upload Station on your computer").
- **Restore:** the dashboard sends a stored blob back to a badge over WebSerial.
- A hacker can fetch their own zip from the frontend (read-only).

### 16.3 Networking view on the dashboard

- Fetch the attendee's blob and **unpack it in the frontend**.
- Use the locally-stored contact list for **names and ordering**.
- On click, pull that person's **live info** via their `claim_id`, so links are always current.

> This is zip-driven by design. It makes the data unqueryable, which is the accepted tradeoff: the alternative is re-deriving badge state server-side, which would make badge sync much harder and require a far more complex model of file-based badge state. Noted here so nobody "fixes" it later without knowing why.

### 16.4 Admin re-flash (P1.5)

A badge admin tool — vibe-coded is fine — that can:

- Look up an attendee by email, pull their stored JSON, and flash it onto a fresh badge.
- Factory-reset badges.
- Erase and re-provision identity (`prov erase confirm`).

Depends on `get`, `prov apply`, and the shared WebSerial library.

### 16.5 Goose games integration

The upload endpoint evaluates quests server-side:

- Number of sponsors visited
- Number of networking bumps
- Number of apps unlocked
- Beat Snake

All four are derivable from the upload blob, so **no extra firmware work** — just make sure each is present in the payload.

### 16.6 Flashing your own code

- Hackers go to `badge.hackthenorth.com/ide` — this is [`tools/badgectl/`](../tools/badgectl/), which **needs hosting, not building.**
- The Lua sandbox loads only `base`, `table`, `string`, `math`, `utf8`, and nils out `dofile`/`loadfile`/`load`/`require` ([`script_app.cpp:121`](../main/apps/script_app.cpp)). The `badge` API surface is in [`lua_bindings.cpp`](../main/apps/lua_bindings.cpp): labels, boxes, config get/set, random, gestures, buttons.
- **Not exposed to Lua today:** LEDs, NFC, radio, raw accelerometer, file I/O, timers, images. If we want hackers making interesting things, LEDs and the accelerometer are the two highest-value additions.
- **No example Lua app is checked in.** Ship two or three, or the IDE opens to a blank page.

### 16.7 Badge website

`badge.hackthenorth.com` needs: hype; a button through to the hacker dashboard for networking info; quick start; deeper instructions; the rules (same as the back of the badge); the customization flow; IDE usage; and the **full unlock list** so people know what they're hunting.

---

## 17. Open questions & decisions register

### Decided

| # | Decision | Section |
|---|---|---|
| 1 | Upload/sync over **BLE relay (BBX1)**, not Wi-Fi on the badge | §11 |
| 2 | Ceremonies beacons are **~12 dedicated ESP32-C3 boxes**, not phones | §12 |
| 3 | Badge Bump sends the **full payload, ack-gated with a 400 ms cap**, residual scraping risk accepted | §10 |
| 4 | Power saving is **light sleep**, not deep sleep | §13 |
| 5 | Solana keys are **self-managed on-device**; not production | §14 |
| 6 | Badge identity is a **JSON file in LittleFS**, not a partition. Loss risks accepted. | §3 |
| 7 | Ceremonies auth is a **shared password checked against a hardcoded hash**. Replay accepted. | §12 |
| 8 | The dashboard networking view is **zip-driven and unqueryable**, deliberately | §16.3 |
| 9 | `badge_id` is a **64-byte string field**; keep to ~16–22 chars in practice | §3 |
| 10 | Provisioning writes `identity.json` **directly**, no staging file | §5 |
| 11 | Onboarding completion **is** the waiver record; stored as a build number in LittleFS | §6 |
| 12 | The ID page is an **ordinary app, registered first and auto-launched on boot** — no launcher pane axis | §7 |
| 13 | Progress totals are **hardcoded**; contacts is a **count, not a fraction** | §8 |
| A | **No transaction signing** — public-key derivation only | §14 |
| B | **Latin-1 display names supported**; fonts must be regenerated to cover the supplement | §4 |
| D | Wake is **Start only**, with an on-screen "Press START to wake" card | §13 |
| E | **Pursue chained ext-adv** (up to ~1580 B frames) | §11 |
| H | Sleep idle timeout is **5 minutes** | §13 |
| I | **Onboarding replays on every re-flash. Accepted** — no build-number freeze for hotfixes. | §6 |

### Open

| # | Question | Blocks | Owner |
|---|---|---|---|
| C | **NFC tag type and locking method** — has procurement lead time. | §15 tag order | Logistics |
| F | **Beacon box count and placement** for Laz coverage. Current plan ~12; confirm at rehearsal. | §12 hardware order | Rehearsal |
| G | Exact **sponsor bay two-floor layout**, and the final **booth count** for the hardcoded progress denominator. | §15 map, §8 | Design |
| J | Final **sponsor booth count** and **Solana Scavenger Hunt count** (5 vs up to 9) to bake into the progress app. | §8 | Product + Solana |

---

## 18. Critical path & sequencing

**Today is 2026-08-01.** An early-September event is ~5 weeks out. This section is triage, not a wish list.

### Week 1 — blocks everything downstream

**No partition table change is needed** — identity is a file, so the fleet cannot split across two flash maps. That removes what would otherwise have been the hardest ordering constraint.

1. **`hal_radio_adv_pause/resume` + `set_adv_interval` + `set_scan_duty`.** Half a day, and it is the true root dependency: the bump privacy fix, the 200 ms bump window, upload fragmentation, and ceremonies all need it.

2. **The bump privacy fix** — ack-gated transmit, 30 ms adv interval, 100% scan duty while the app is open. Cheap once #1 lands, and the current code is **actively leaking attendee PII**. The short transmit is *only* viable with the retimed radio; shipping it without the retiming would make bumps fail silently (§10).

3. **`hal_identity` + `prov apply`.** Unblocks provisioning, onboarding, the ID page, bump payloads, and the Solana request body — nearly everything else depends on identity existing.

4. **Two-board BBX bench test** — raise the frame cap toward 1580 and drop the adv interval to 30 ms on a badge and a relay on a desk. Measure frame loss at 10 cm, 1 m, 5 m. Decides whether upload stations feel instant, and whether a worst-case 347-byte bump packet is transmissible (§10).

### Parallel tracks after week 1

| Track | Est. | Depends on |
|---|---|---|
| Provisioning: `prov` commands, LED/LCD receipt, registration laptop app | 1–2 wk | `hal_identity` |
| Onboarding + waiver + ID page app | 1–3 wk | `hal_identity` |
| BBX transfer + relay reassembly + station UX | 2–4 wk | bench test #4 |
| **Ceremonies** — beacon mode, WS admin app, badge listener | 2–4 wk | **nothing** — fully independent, shares only `bbx_proto.h` |
| Solana crypto + backend reconciliation | 1–2 wk | `hal_identity`, question A |
| Sponsor map | 1 wk | question G, tag order |
| Sleep manager | 1 wk | question D |

Ceremonies is the one large track with **no dependency on the identity work** — start it in parallel on day one.

### IRL tests, ordered by consequence

1. **RF congestion with 20–50 badges advertising in one room.** The highest-consequence unknown in this entire document. Everything here assumes a badge can hear a beacon; at 1000 badges advertising every 200 ms across three primary channels, the collision rate is a real question. **Needs hardware in hand by mid-August.** Measure BBX1 frame loss and ceremony lock-on time in a crowded room. Mitigations if it's bad: lengthen badge adv intervals outside stations, move ceremony frames to the 2M PHY, add more beacon boxes.
2. **Timed 100-badge provisioning dry run** with a real operator. If the cycle is 45 s/badge, 1000 badges is 12.5 person-hours — **staff registration from a measured number, not a guess.** Also validates that the LED/LCD receipt is readable in a queue.
3. **Badge bump tuning** — the full matrix in §10, early September in Waterloo.
4. **GPIO7 / HC165-LOAD-low wake test** — 10 minutes with a scope, decides Start-only vs any-button wake.
5. **End-to-end upload station with a queue of 5 people** — the `XFER_GRANT` lease and the "2 ahead" UI only get exercised under contention.
6. **LittleFS mount time with 200 contact files** — affects boot and station latency.

### Cut list, in order

If the schedule slips, cut in this order:

1. **Downlink sync** — make uploads one-way, push nothing back. (Loses live networking refresh and server-pushed unlocks.)
2. **The progress app's server-fed categories** — derive everything locally.
3. **Light sleep** — keep the `DIM` tier only (LVGL to 10 Hz, LEDs off). Costs battery life, not function.
4. **The sponsor bonus game.**

**Do not cut:** provisioning, onboarding/waiver, the ID page, ceremonies. Those are what attendees actually see and touch.

---

## Appendix: corrections to existing repo docs

Found while auditing. These are wrong today and will mislead whoever picks up the work:

| File | Claim | Reality |
|---|---|---|
| ~~`CLAUDE.md:25`~~ | ~~"`requires_unlock()` defaults to true"~~ | **Fixed.** Access is now the single `App::access()` tier + `launcher_slugs`. |
| `CLAUDE.md:32` | "Add to `main/CMakeLists.txt` SRCS list" | There is no SRCS list — it uses `SRC_DIRS` globbing, so new files are picked up automatically. |
| `CLAUDE.md:33` | "Page 1 is the first 16 slots after the scanner" | Registry order only determines *Default-tier* ordering; everything else follows the persisted `launcher_order` CSV, which users reorder at runtime. |
| `CLAUDE.md:20` | references `tools/badgectl/` for the web UI | Both `tools/badgectl/` (Lua IDE) and `tools/web-flasher/` (firmware flashing) exist and are different things. |
| `launcher_slugs.h:13` | "Migrates from the legacy `unlocked` key on first load" | **No migration code exists.** |
| `app_card.cpp:21` | default for `url` | is the string `"Unset Email"` — copy-paste bug. |
| `app_power.cpp:97` | "on this build PM is off" | `CONFIG_PM_ENABLE=y` is now set. Also `app_power` is commented out of the build entirely (`main.cpp:83`). |

Also note: `app_power`, `app_bump`, `app_buttons`, `app_nfc`, `app_lights`, `app_kitchen`, and `app_simon` are **compiled and linked but never registered** (`main.cpp:83-89`), so they're unreachable even in debug mode. Either register them or drop them — they're costing flash for nothing.
