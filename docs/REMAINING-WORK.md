# Remaining Work

Gap list against [BADGE-EVENT-EXPERIENCE.md](BADGE-EVENT-EXPERIENCE.md). Section numbers (§) refer to that document.

**Last updated:** 2026-08-31 - **Build state:** clean, 1.68 MB image, 39% of the app partition free. `tools/sync_station` builds clean at 1.16 MB, 24% free.

---

## 1. What already works

Everything here builds and is wired into `app_main`. None of it has been verified on hardware yet — see §4.

| Area | Files | Spec |
|---|---|---|
| Radio timing control (`adv_pause/resume`, `set_adv_interval_ms`, `set_scan_duty_ms`, `reset_timings`) | [hal_radio.cpp](../main/hal/hal_radio.cpp) | §10, §11, §12 |
| Badge identity from `/littlefs/identity.json` | [hal_identity.cpp](../main/hal/hal_identity.cpp) | §3 |
| Provisioning console: `prov apply/show/mac/erase` + LED receipt | [hal_console.cpp](../main/hal/hal_console.cpp) | §5 |
| BMP2 bump wire format (TLV, priority-ordered, forward-compatible) | [bump_proto.cpp](../main/apps/bump_proto.cpp) | §10 |
| Badge Bump: ack-gated TX, 30 ms adv, 100% scan, 500 mg jerk, RSSI gate | [app_badge_bump.cpp](../main/apps/app_badge_bump.cpp) | §10 |
| Contacts keyed on `badge_id`, peer input sanitised | [contacts_store.cpp](../main/apps/contacts_store.cpp) | §10, §11 |
| Badge ID page (boot target, first tile, QR + details) | [app_badge_id.cpp](../main/apps/app_badge_id.cpp) | §7 |
| Light sleep, Start-button wake, on-screen sleep card | [hal_sleep.cpp](../main/hal/hal_sleep.cpp) | §13 |
| First-boot onboarding, tutorial, rules, build-number gate | [app_onboarding.cpp](../main/apps/app_onboarding.cpp) | §6 |
| Progress app (hardcoded totals, locally-derived counts) | [app_progress.cpp](../main/apps/app_progress.cpp) | §8 |
| Solana wallet: ed25519 via libsodium, base58, entropy gate | [solana_wallet.cpp](../main/apps/solana_wallet.cpp) | §14 |
| Solana Scavenger Hunt reconciled to Solana's real request/response contract | [app_blindbox.cpp](../main/apps/app_blindbox.cpp) | §14 |
| Filesystem → JSON snapshot, binaries stubbed; `snapshot` console command | [fs_snapshot.cpp](../main/apps/fs_snapshot.cpp), [json_util.cpp](../main/apps/json_util.cpp) | §11, §16.1 |
| BBX1 bulk transfer: selective repeat, 32-byte bitmap, GRANT lease | [bbx_proto.h](../main/apps/bbx_proto.h), [bbx_xfer.cpp](../main/apps/bbx_xfer.cpp) | §11 |
| Sync app (Public), per-failure copy, wake lock only while transferring | [app_sync.cpp](../main/apps/app_sync.cpp) | §11 |
| Sync station: reassembly, lease, 20 KB chunked HTTPS upload | [tools/sync_station/](../tools/sync_station/) | §11 |

---

## 2. Not built

Ordered by how much else depends on them.

### 2.1 Ceremonies — `NOT STARTED` (§12)

The largest remaining piece, and **completely independent of everything else** — it shares only `bbx_proto.h`. Can start immediately, in parallel.

| Piece | Where | Notes |
|---|---|---|
| `BBX_TYPE_CEREMONY` frame + `bbx_ceremony_v1_t` | both copies of `bbx_proto.h` | 32-byte body, 50-byte frame — fits one primary-channel PDU |
| Badge-side listener + effect renderer | new `app_ceremony.cpp` | Clock **slew, never snap** — 12 beacons at different offsets will stutter otherwise |
| Beacon mode for the relay | `tools/relay_firmware/` | Add `CONFIG_RELAY_MODE_CEREMONY` + `ws_client.c`. Same binary, same flashing procedure |
| Admin web control app | new | Auth required, per-box heartbeat, reconnect backoff, big ALL STOP |
| Poll-through-sleep hook | [hal_sleep.cpp](../main/hal/hal_sleep.cpp) | **The seam already exists** — the timer-wake branch at the bottom of `hal_sleep_tick()` is where the 3-minute beacon listen goes |
| Password auth (`pw_hash` vs hardcoded SHA-256 prefix) | badge + beacon | Replay is accepted; don't describe the channel as authenticated |

Hardware: ~12 ESP32-C3 beacon boxes need ordering. Placement confirmed at rehearsal.

### 2.2 Upload stations & BBX1 fragmentation — `BUILT`, unverified on hardware (§11)

Upload works end to end in simulation. What is left:

| Piece | Where | Notes |
|---|---|---|
| **Hardware bring-up** | — | Nothing in this row's section has run on real silicon. Order in `tools/sync_station/README.md` |
| **Dwell and timeout tuning** | [bbx_xfer.cpp](../main/apps/bbx_xfer.cpp) | Constants are reasoned, not measured. `xfer` on the console prints the readings |
| Sync app icon | `assets/icons/sync.png` | Ships on the `"SYN"` text fallback today; needs 96×96 artwork |
| Downlink / sync-down | badge + station | `want_downlink` is wired and always 0. Not started |
| Field restore (`BBX_TYPE_IDENT_REQ`) | badge + backend | Type reserved, nothing implemented. Depends on registration recording `mac → badge_id` |
| Mailbox app (explains the system) | new `app_mailbox.cpp` | §8. The Sync app covers the mechanism, not the story |
| HAPI resolver | backend | Service token, authorization guarded, triggers goose games quests |
| Station hardware | — | ESP32-C3 boxes need ordering, same as the ceremony beacons |

Note the payload is the **whole filesystem**, not the curated `{badge_id, unix}`
blob §11 budgeted at 1.3 KB — see the deviation note at the top of that section.
### 2.3 Sponsor tables — `NOT STARTED` (§15)

| Piece | Notes |
|---|---|
| Sponsor map app | Two floors this year — needs a floor toggle or stacked view |
| Tap handling in the scanner | Writes one file per booth to `/littlefs/config/sponsors/` — **this contract is already what [app_progress.cpp](../main/apps/app_progress.cpp) counts** |
| Sponsor data table on-badge | Nice names, logos, links. Tag carries only `sponsor_name` + `table_number` |
| Tag programming + locking | ⚠️ `hal_nfc` is **read-only, no NDEF writer**. Needs an external tool and a tag-type decision — this has procurement lead time |

### 2.4 Content apps — `NOT STARTED` (§8)

Small, independent, good parallel work.

- **Welcome booklet** — TL;DR of event info + QR to the full booklet.
- **Info** — QR to `badge.hackthenorth.com` and the dashboard, including the schedule.
- **Ticket** — trivial; redirects to the ID page app.
- **Mailbox** — see §2.2.

### 2.5 Smaller firmware gaps

| Gap | Spec | Notes |
|---|---|---|
| **Boot splash** (HTN + Solana logos, 1 s) | §6 | Nothing exists today; badge goes straight to onboarding or the ID page |
| **Latin-1 font rebuild** | §4 | ⚠️ Built-in Montserrat is ASCII-only. `José` renders blank **until this is done**. Build-config change, but must land before the first real name is provisioned |
| **Pal background on the ID page** | §7 | Second visual state after unlocking characters |
| **Pal icons + equip flow + chain-explorer QR** | §14 | `chain_link` is already stored; nothing renders it yet |
| **LCD provisioning receipt** | §5 | LED receipt is done; the full-screen name/role/ID confirmation is not |
| **`get <path>` console command** | §16.1 | Badge→host download. `cat` is not binary-safe. **Backup, restore, and the admin tool all block on this** |
| **`app_card` is now vestigial** | — | Still reads `card.cfg`; superseded by the ID page. Remove or repoint at identity |

### 2.6 Off-badge

Not in this repo, listed so nothing falls through.

- Registration laptop app (WebSerial; must record `mac → badge_id`).
- Shared WebSerial library extracted from `tools/badgectl/web/app.js`.
- Dashboard networking view (zip-driven, deliberately).
- Admin re-flash / factory-reset tool (P1.5).
- Goose games quest hooks on the upload endpoint.
- `badge.hackthenorth.com` — hype, quick start, rules, unlock list, IDE. `tools/badgectl/` needs **hosting, not building**.
- RSVP form changes + extended profile claim fields (§4).

---

## 3. Values that must be set before the event

Wrong values here are silent — nothing fails, the numbers are just incorrect on every badge.

| What | Where | Current |
|---|---|---|
| Sponsor booth count | `kSponsorTotal`, [app_progress.cpp](../main/apps/app_progress.cpp) | `18` — placeholder |
| Solana Scavenger Hunt count | `kScavengerHuntTotal`, [app_progress.cpp](../main/apps/app_progress.cpp) | `5` — may become up to 9 |
| Pal names | `kPals`, [app_blindbox.cpp](../main/apps/app_blindbox.cpp) | Placeholders; only `"2" → Patch` confirmed |
| Ceremonies password | not yet written | §12 |
| `BADGE_BUILD` | env var at configure time | Unset → `0`, so dev rebuilds don't replay onboarding |

---

## 4. Needs hardware verification

Nothing below has run on a badge. Ordered by consequence.

1. **RF congestion with 20–50 badges advertising in one room.** The highest-consequence unknown in the whole project. Everything assumes a badge can hear a beacon. Needs hardware in hand by mid-August.
2. **`esp_light_sleep_start()` from inside the LVGL timer callback.** [hal_sleep.cpp](../main/hal/hal_sleep.cpp) sleeps once per tick rather than looping, specifically to bound how long the LVGL port lock is held — but this is the piece most likely to behave differently on real silicon.
3. **`ble_gap_ext_adv_configure()` on a live-but-stopped instance.** The retune path in [hal_radio.cpp](../main/hal/hal_radio.cpp) stops, reconfigures, re-applies the cached payload, and restarts. If the controller doesn't like it, bump timing and upload throughput both rest on this.
4. **Bump end-to-end between two badges**, including the ack-gated early stop. Full test matrix in §10 — needs the early-September Waterloo session.
5. **Chained extended advertising (~1580-byte frames).** Decides whether a full upload is 1 frame or 6, and whether a worst-case 347-byte bump packet is transmissible.
6. **Timed 100-badge provisioning dry run.** Staff registration from a measured number, not a guess.

---

## 5. Known traps

Written down because each will cost a day if rediscovered from symptoms.

- **The relay's dedupe LRU will eat every upload fragment.** `seen_before_locked()` keys on `(badge, req_id)`, and all frames in one transfer share a `req_id`. Presents as *"uploads always stall after the first frame."* `tools/sync_station` pre-empts this by having no LRU at all — the receive bitmap is the dedupe. If a station ever also handles `BBX_TYPE_REQ`, gate the LRU to that type.
- **A repeated `XFER_OPEN` must not reset the session.** The badge's advertisement repeats tens of times a second, so most OPENs a station sees are for a transfer already in progress. Re-allocating on each would clear the bitmap every 30 ms and present as *a permanent 0%*.
- **A peer's advertisement is a value, not a message.** Reading it twice does not mean it was sent twice, and reading it right after changing your own does not mean the peer has reacted yet. This bit the STAT path: after a repair sweep the badge could act on the bitmap from *before* that sweep, see an unchanged `recv_count`, and fail a working transfer as "Signal too weak". Anything inferring progress from a peer's register needs a settling window — `kSweepDoneHoldTicks` in [bbx_xfer.cpp](../main/apps/bbx_xfer.cpp).
- **A short bump transmit needs the retimed radio.** At the default 200 ms adv interval and 20% scan duty, a ~200 ms burst emits roughly one advertising event with a ~20% chance of being heard. Bumps fail silently. The retune is in `on_enter` — don't remove it.
- **`factory_reset` must never reach `/littlefs/identity.json` or `/littlefs/solana.json`.** Both sit at the LittleFS root precisely so `rm -rf /littlefs/config` can't touch them. The natural "make it more thorough" edit destroys identity fleet-wide.
- **"Erase Flash" in the web flasher destroys identity and wallets.** It uses `eraseAll: false` today. Never expose the erase button.
- **Both copies of `bbx_proto.h` must change together** (`main/apps/` and `tools/relay_firmware/main/`). Drift fails silently as *"the badge never gets a response."*
- **The contact file format changed.** Old contact files on dev badges won't parse — they show as nameless entries. `factory_reset` any test unit.
- **`esp_random()` is only a true RNG with the radio up.** Wallet generation already enforces this; any future key or nonce must too.
- **New source files need `idf.py reconfigure`.** `SRC_DIRS` globs at configure time, so a new `.cpp` fails to link until then.
- **`idf.py` is a shell alias**, so `. activate && idf.py build` fails in a non-interactive shell — the alias isn't expanded. Call the venv python and `tools/idf.py` by path.

---

## 6. Suggested order

Three tracks that don't block each other:

| Track | Start with |
|---|---|
| **A — Ceremonies** | Frame type → badge listener → beacon mode → admin app. Order beacon hardware now. |
| **B — Sync** | Built through the first end-to-end path (`snapshot` covers the `get` need). Next: hardware bring-up per `tools/sync_station/README.md`, then timing tuning, then mailbox app → HAPI resolver |
| **C — Content & polish** | Latin-1 font rebuild → boot splash → info/booklet/ticket apps → sponsor map → pal art. |

Do first, regardless of track: the **Latin-1 font rebuild** (blocks provisioning real names). The badge→host download that blocked backup, restore and admin tooling is now the `snapshot` console command, which prints the whole filesystem as JSON with binaries stubbed rather than mangled.

If the schedule slips, cut in this order: downlink sync → server-fed progress → light sleep (keep `DIM` only) → sponsor bonus game. Keep provisioning, onboarding, the ID page, and ceremonies — those are what attendees actually touch.
