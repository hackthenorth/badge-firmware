# Solana Scavenger Hunt relay firmware

A dumb byte pipe between the badges and the internet.

```
badge app  --BLE(BBX1)-->  THIS  --HTTPS POST-->  webhook
badge app  <-BLE(BBX1)---  THIS  <---HTTP 200---  webhook
```

The badge has no WiFi and no HTTP stack, so it can't reach a server on its own.
It broadcasts its request over BLE; this relay picks it up, POSTs the body
verbatim to a configured URL, and broadcasts the reply straight back.

**It never parses the payload.** The badge builds the request JSON and parses
the response JSON. As far as the relay is concerned both are opaque bytes.

## This is a separate ESP-IDF project

It is not part of the badge build. Build it from inside this directory:

```bash
source ~/.espressif/tools/activate_idf_v5.5.3.sh
cd tools/relay_firmware
idf.py set-target esp32c3      # first time only
idf.py menuconfig              # Solana Scavenger Hunt Relay -> SSID, password, webhook URL
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Target hardware is an ESP32-C3 — same chip as the badge, so any spare dev board
works. It needs USB power and WiFi; it is not a battery device.

## Configuration

Everything lives under **Solana Scavenger Hunt Relay** in `menuconfig`:

| Option | What it is |
|---|---|
| `RELAY_WIFI_SSID` / `RELAY_WIFI_PASSWORD` | Network to join (WPA2-PSK). |
| `RELAY_WIFI_STA_MAC` | Custom STA MAC for whitelisted networks; empty keeps the chip MAC. |
| `RELAY_WEBHOOK_URL` | Where every request gets POSTed. E.g. `https://your-app.vercel.app/api/open` |
| `RELAY_BOX_ID` | This station's id, broadcast in every beacon. Max 39 chars. |
| `RELAY_SELFTEST_ON_BOOT` | POST one canned request at startup and log the result. |
| `RELAY_SCREEN_ENABLE` | LCD status screen (same panel as the badge). Status-only; the relay runs headless without it. |

`RELAY_BOX_ID` is what tells a badge which box it walked up to — the badge
echoes it back as the `box` field of its request. The relay never parses the
payload; it only ever sees `REQ` frames.

The webhook URL lives here rather than in the badge's request so you can
repoint the server without reflashing badges, and so nobody in BLE range can
make the relay POST to a host of their choosing.

## Station ID + MAC editor

The box carries the badge's buttons (same 74HC165 wiring). Up opens the
station-ID editor, Down the WiFi MAC editor; Left/Right adjust the open row.
8 s without a press exits back to the status screen. Both settings persist
in NVS across reboots.

Station slugs, in order:

`solana-booth`, `hardware-hub`, `extended-bay`, `mentor-cafe`,
`third-floor`, `fourth-floor`, `fifth-floor`, `seventh-floor`,
`solana-booth-final`

The beacon, screen footer, and heartbeat follow an ID change immediately.
`RELAY_BOX_ID` is only the fallback boot selection — set it to the slug for
where the box lives.

The MAC editor starts unconfigured (shown with `auto`). Setting it uses the
event's fixed `24:0A:C4:C8:46` prefix and lets Left/Right pick the last byte
from `A0` to `B3` (20 provisioned values). It does not use the board's factory
MAC prefix. Only the selected last byte is saved, so existing saved selections
also use the correct event prefix after updating the firmware. A newly-set MAC
needs a reboot: the WiFi MAC can only be applied before the interface starts.
Precedence at boot is NVS editor value, then `RELAY_WIFI_STA_MAC`, then chip
default. Assign each box a distinct address; `B2` and `B3` are used by the two
sync stations.

## Lights

The box drives the badge's WS2812 strip (GPIO3) as dim status lights:
green idle/ok, gold while a POST or response is in flight, red on fault or
no WiFi. Output passes through the badge's gamma curve plus an extra
divide-by-4, so full brightness lands around 16/255 — a glow, not a lamp.
If the strip is absent the box runs headless as usual.

## Heartbeat

`main/heartbeat.c` POSTs this box's `RELAY_BOX_ID` to the Supabase
`touch_relay_heartbeat()` RPC every 3 minutes (same `badge-relay` project as
the relay server). A pg_cron watchdog Slacks when a box goes stale — see the
`_heartbeats` migration in `tools/relay_server/supabase/migrations/`.

Needs `RELAY_SUPABASE_ANON_KEY` from menuconfig (Project Settings → API);
with it empty the task logs once and exits. Watch for `heartbeat ok` in the
monitor log after flashing.

## How it works

Three execution contexts, and the split is the whole design:

| Context | Job |
|---|---|
| NimBLE host task (`on_frame`) | Validate magic, dedupe, enqueue. Never blocks. |
| `bbx_worker` task | The HTTPS round trip. Blocks for seconds. |
| `bbx_ticker` task (200 ms) | Retires a response back to a `BEACON`. |

An HTTPS request from the host callback would stall the BLE stack for the whole
round trip, so it gets handed off.

**Deduplication.** BLE advertising is a continuous re-broadcast — the badge
repeats its request every ~200 ms for up to 12 s. That repetition *is* the
retry mechanism, but it means a single tap would fire dozens of POSTs without
the 16-entry `(badge, req_id)` LRU in `main.c`.

**Response lifetime.** After a POST the reply is advertised until the badge
sends an `ACK` (normally well under a second) or 6 s elapse, whichever comes
first. The ACK is what keeps throughput reasonable when there's a queue at a
station — without it each attendee occupies the relay for the full 6 s.

**When the queue is full** (4 jobs deep) the relay answers `BBX_ST_BUSY`
immediately, so the badge shows "Relay is busy" rather than sitting through a
12 s timeout.

## Status screen

The relay carries the same LCD as the badge (ST7789 320x240 over SPI, same
pins as `main/pins.h`) and renders it with the same graphics system: LVGL
through `esp_lvgl_port`, vendored from the badge's `managed_components`
(`EXTRA_COMPONENT_DIRS` in the project `CMakeLists.txt`, so both firmwares
always build against identical LVGL versions). `main/screen.c` mirrors the
sync station's chrome — title, bordered panel, status + detail labels — so
both boxes show the same vocabulary in the same visual language.

What it shows, driven by the 200 ms ticker in `main.c`:

| Screen | Meaning |
|---|---|
| `<id>` / `boot` | starting up |
| `<id>` / `wifi...` | joining the AP |
| `<id>` / `no wifi` | no IP; retrying in the background |
| `selftest` / `POST` then `ok` / short error | boot self-test verdict, held ~1 s |
| `<id>` / `ready` | idle, beaconing |
| `<id>` / `ok N` (`ok N busy M`) | idle, with served (and refused) counts |
| `POST` / `<req id>` | webhook round trip in flight |
| `RESP` / verdict | response on the air, waiting for the badge's ACK |

The display is status-only: if it is missing or the bus fails, the relay
logs once and runs headless. `RELAY_SCREEN_ENABLE=n` removes it at compile
time. `screen_show()` diffs so a steady state costs no redraws.

## Bring-up order

Test each layer before the one above it depends on it.

1. **Server alone.** `cd ../relay_server && npx vercel dev`, then curl it. See
   that README.
2. **WiFi + HTTP, no BLE.** Flash with `RELAY_SELFTEST_ON_BOOT=y` and watch
   `idf.py monitor`. You want `self-test OK` after an HTTP 200. The test
   sends `box=selftest`, `user_id=relay-selftest`, and `name=relay` using
   the current webhook contract (the old `badge` field is rejected). This
   isolates credentials, DNS, and the cert bundle from anything radio-related.
3. **BLE receive.** With the relay running, open the **Badge Bump** app on a
   badge. Its frames share the same `0xFFFF` manufacturer-data channel, so the
   relay will see them and silently reject them on the magic check — which
   proves scanning and AD parsing work before any `BBX1` traffic exists. A
   phone BLE scanner will also show the relay's `BEACON`.
4. **Full flow.** Open the Solana Scavenger Hunt app and walk up to the
   station. The badge fires on its own when the signal is strong, the "relay"
   indicator turns green when it hears the beacon, and the item shows up in a
   second or two.

## Known limits

- **TLS needs heap after BLE starts.** Keep `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`:
  fixed TLS record buffers can exhaust the C3's heap during certificate
  verification with WiFi, NimBLE, LVGL, and heartbeat tasks running. Error
  `0x4290` from certificate verification includes an MPI allocation failure.
  The `POST heap` log reports free memory and the largest block before a
  connection; certificate verification must stay enabled. Badge POSTs and
  heartbeats share a mutex so they cannot allocate two TLS sessions at once;
  a heartbeat deferred by a badge POST retries after one second.

- **One in-flight request at a time**, four queued. Roughly one attendee every
  1–2 s per relay. A busy station wants a second relay.
- **The relay stops beaconing while it holds a response** (up to 6 s, normally
  under 1 s thanks to the ACK). A second badge in proximity mode hears no
  beacons during that window and won't trigger, which effectively serialises
  attendees at a station. Fixing it properly needs a second advertising
  instance; `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES` is 1 today.
- **WiFi and BLE share one 2.4 GHz radio** on the C3. With
  `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` they take turns, so the effective scan
  duty drops below the configured 100% — most sharply *during* an HTTP request.
  The badge's ~60 repeats give it plenty of chances, but this is the most
  likely source of flakiness, which is why dropped and duplicate frames get
  logged.
- **No authentication on the BLE link.** Anyone in range can forge a request
  with any badge MAC, or spoof a response. Fine for a proof of concept; a
  shared HMAC over the frame is the obvious fix if this ever gates something
  real.
- `main/bbx_proto.h` is a **duplicate** of `main/apps/bbx_proto.h` in the badge
  firmware, because a separate IDF project can't include from it. Edit one
  without the other and the failure looks like "the badge never gets a
  response".
