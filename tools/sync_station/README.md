# Badge sync station

Receives a badge's credential envelope and filesystem snapshot over BLE and POSTs the JSON to HackerAPI,
so a lost or bricked badge can be restored and the backend can see what a badge
holds.

ESP32-C3, ESP-IDF v5.5.3, mains powered. A **separate IDF project** from the
badge firmware at the repo root and from `tools/relay_firmware` — build it from
inside this directory.

## Why the badge doesn't just use WiFi

It has no WiFi, no TCP/IP and no HTTP stack. Adding them costs ~400-600 KB of
flash, SSID provisioning at registration, WiFi/BLE coexistence on the C3's
single radio, and a real battery hit. So the badge advertises and a mains-
powered box does the internet part. See `docs/BADGE-EVENT-EXPERIENCE.md` §11.

## Relationship to the relay

`tools/relay_firmware` is the same idea for the Solana Scavenger Hunt: a BLE↔HTTPS bridge.
The station is a separate binary rather than a mode of the relay because what it
does with the bytes is entirely different — a relay forwards one 232-byte
request and broadcasts the reply, a station reassembles ~90 frames into one
~20 KB body and returns the latest profile from that POST response to the badge.

`ble_link.c` and `wifi_sta.c` are near-verbatim copies of the relay's.
`net_post.c` is not — see below.

Both projects and the badge carry a copy of `bbx_proto.h`, and all three must
declare the same protocol. `tools/check-bbx-proto.py` enforces that in CI. It
compares declarations and ignores comments, because the copies deliberately
carry different prose pointing at each other.

## Build and flash

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
cd tools/sync_station
idf.py set-target esp32c3          # first time only
idf.py menuconfig                  # Badge Sync Station -> SSID, password, URL, id
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

`idf.py` is a shell alias, so `. activate && idf.py build` fails in a
non-interactive shell — the alias isn't expanded. In a script, call the venv
python and `tools/idf.py` by path:

```bash
"$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" build
```

## Configuration

All compile-time, under **Badge Sync Station** in `menuconfig`:

| Option | Default | Notes |
|---|---|---|
| `SYNC_WIFI_SSID` / `SYNC_WIFI_PASSWORD` | *(empty)* | Set both in menuconfig; WPA2-PSK; empty password = open |
| `SYNC_UPLOAD_URL` | HackerAPI `/v3/badge-data/upload` | Fixed destination for the one POST; never taken from a badge |
| `SYNC_AUTH_TOKEN` | *(empty)* | Raw station password, sent as the JSON `password` parameter exactly as configured (no Bearer prefix); empty disables uploads |
| `SYNC_STATION_ID` | `sync-01` | Idle beacon body only (local radio metadata, never sent to HackerAPI); max 39 chars |
| `SYNC_MAX_BLOB_BYTES` | 32768 | Larger transfers refused before allocating |
| `SYNC_ADV_INTERVAL_MS` | 30 | Advertising interval during a transfer |
| `SYNC_HTTP_TIMEOUT_MS` | 15000 | Covers DNS, TLS and the whole POST |
| `SYNC_SELFTEST_ON_BOOT` | n | Synthetic fixture with no registered claim/token; leave disabled against HackerAPI |

The endpoint lives here rather than in the badge's request so the server can be
repointed without reflashing badges, and so nobody in BLE range can make the
station POST to a host of their choosing.

## Station ID select

The desk carries the badge's buttons (same 74HC165 wiring). Up opens the
station-ID editor and Left/Right step between `sync-01` and `sync-02`;
Down shows the WiFi MAC as a readout; 8 s without a press exits back to
the status screen. The beacon, screen, and heartbeat follow the selection,
which persists in NVS across reboots. `SYNC_STATION_ID` is only the
fallback boot selection.

The WiFi MAC follows the station: `sync-01` uses `24:0A:C4:C8:46:B2` and
`sync-02` uses `24:0A:C4:C8:46:B3`. Both use the event's registered prefix,
regardless of the board's factory MAC. Reserve these two addresses for the
sync stations when assigning relay MACs. Switching IDs needs a reboot for
the MAC to follow (the WiFi MAC can only be applied before the interface
starts). The ID-derived MAC takes precedence over `SYNC_WIFI_STA_MAC`.

## Lights

The desk drives the badge's WS2812 strip (GPIO3) as dim status lights:
green idle/DONE-ok, gold while receiving, uploading, or sending the profile
back, red on failure or no WiFi. Output passes through the badge's gamma
curve plus an extra divide-by-4, so full brightness lands around 16/255 — a
glow, not a lamp. If the strip is absent the desk runs headless as usual.

## Heartbeat

`main/heartbeat.c` POSTs this station's `SYNC_STATION_ID` to the Supabase
`touch_sync_heartbeat()` RPC every 3 minutes (same `badge-relay` project as
the relay server). A pg_cron watchdog Slacks when a station goes stale — see
the `_heartbeats` migration in `tools/relay_server/supabase/migrations/`.

Needs `SYNC_SUPABASE_ANON_KEY` from menuconfig (Project Settings → API);
with it empty the task logs once and exits. Watch for `heartbeat ok` in the
monitor log after flashing.

## Bring-up order

Work outward from the thing most likely to be broken.

1. **Host tests.** Run `python3 tools/test-sync-upload.py` and
   `python3 tools/test-sync-radio.py` from the repo root. These exercise the
   real credential/profile code, HTTP forwarding, and both radio engines with
   loss and repetition. Keep legacy synthetic boot self-tests disabled against
   HackerAPI; they have no registered credential.
2. **Beacon visible.** A registered badge with the Sync app open should list the
   station by its `SYNC_STATION_ID`.
3. **End to end.** Expect OPEN → GRANT → snapshot DATA/STAT repair → one POST →
   PROFILE_DATA/PROFILE_ACK chunks → DONE → profile applied → Synced. Verify the
   backup answer timestamp in Mydot and the current name/contact fields on the
   badge. The backup contains the identity from before this profile update.
4. **Retry.** Interrupt reception and reconnect. A committed backup remains saved;
   retrying repeats the one POST and idempotently applies the latest profile.

## Status screen

The station carries the same LCD as the badge (ST7789 320x240 over SPI, same
pins as `main/pins.h`) and renders it with the same graphics system: LVGL
through `esp_lvgl_port`, vendored from the badge's `managed_components`
(`EXTRA_COMPONENT_DIRS` in the project `CMakeLists.txt`, so both firmwares
always build against identical LVGL versions). `main/screen.c` mirrors the
badge Sync app's chrome — title, bordered panel, status + detail labels,
progress bar — so both ends of a transfer show the same vocabulary in the
same visual language.

What it shows, driven by the 200 ms ticker in `main.c`:

| Screen | Meaning |
|---|---|
| `<id>` / `boot` | starting up |
| `<id>` / `wifi...` | joining the AP (blocks up to 20 s) |
| `<id>` / `no wifi` | no IP; retrying in the background |
| `selftest` / `POST` then `200 ok` / `HTTP nnn` / short error | boot self-test verdict, held ~1 s |
| `<id>` / `ready` | idle, beaconing |
| `RX` / `n/N` + bar | receiving transfer frames |
| `POST` / `nnKB` | snapshot reassembled, HTTPS upload in flight |
| `SYNC` / `n/N` + bar | sending the returned profile back to the badge |
| `DONE` / `ok` or short error | `XFER_DONE` on the air, waiting for the badge's ACK; an endpoint refusal shows its numeric code (`HTTP 401` wrong station password, `403`/`404`/`409` registration, `422` overlong profile, `429` rate limited); a transport failure shows the network error (`errno 104`, `tls 0x7f00`) |

The display is status-only: if it is missing or the bus fails, the station
logs once and runs headless, retrying every 5 s. `SYNC_SCREEN_ENABLE=n`
removes it at compile time. The ticker reads transfer state through
`xfer_rx_snapshot()` — one lock, no work on the NimBLE host task — and
`screen_show()` diffs so a steady state costs no I2C traffic.

A DNS/TCP/TLS failure retries the POST up to 3 times, 2 s apart, while the
total stays inside the badge's ~20 s answer wait — an HTTP refusal is an
answer, not a retry. The serial log shows each attempt and the final
`request done` line names the last network error (`net=...`).

## Things that will bite

- **A running advertiser can still fail to update its payload.** On the badge,
  `ble_gap_ext_adv_set_data: 519` is HCI memory-capacity-exceeded (0x07 wrapped
  by NimBLE), not weak RF. `radio` retains the error and reports both ordinary
  and DMA-capable heap; the controller specifically needs the latter.
  `adv_active=1` is only host bookkeeping. Keep the badge's MSYS buffer pools
  bounded so the controller has heap left while the snapshot file and profile
  response buffer are open. Watch both serial logs when diagnosing this;
  the badge host tool strips firmware log lines from console command output.
- **The LCD and TLS share the station's RAM budget.** Keep display stripes
  small and BLE buffers sized for broadcast-only traffic. Otherwise the
  snapshot can arrive correctly but `mbedtls_ssl_setup` fails with `-0x7F00`
  (allocation failure) before any HTTP response. The station uses 10-row DMA
  stripes and a 16 KB LVGL pool for its single status screen.
- **`esp_http_client_write()` short-writes on a 20 KB body.** It returns how
  many bytes it took. The relay checks `written != body_len` and errors, which
  is correct only because its bodies fit one TLS record. `net_post.c` here
  loops. Copying the relay's version back over this would fail nearly every
  upload.
- **Do not port the relay's dedupe LRU.** It keys on `(badge, req_id)` and every
  DATA frame in a transfer shares one `req_id`, so it would drop frames 1..N-1
  and present as *"uploads always stall after the first frame."* The receive
  bitmap is the dedupe; setting an already-set bit is a no-op.
- **`XFER_OPEN` arrives tens of times per second.** The badge's advertisement is
  a register that repeats until overwritten, not a queue. A repeated OPEN
  matching the live session must re-broadcast the existing GRANT — never
  re-allocate, never reset the bitmap.
- **Stop the badge's advertisement on failure or cancellation.** Leaving a
  stale OPEN on the air lets it acquire a new lease after the old one expires;
  the station then shows `RX 0/N` while the badge looks idle. Final ACKs get a
  short hold before stopping too. Scanning stays enabled for live discovery.
- **One advertising instance means one session at a time.**
  `MAX_EXT_ADV_INSTANCES=1` on both sides. The GRANT is a lease; without it two
  badges at one station corrupt each other's transfers.
- **The BLE link is not authenticated.** Anyone in range can forge a frame with
  any badge MAC. The snapshot includes the Solana private key. This is accepted
  for the event (§14) but should not be described as a secure channel.

See [credential provisioning and transport](../../docs/BADGE-UPLOAD.md) for the required HTNSYNC1 envelope, JSON request parameters, coordinated badge/station rollout, and local development.
