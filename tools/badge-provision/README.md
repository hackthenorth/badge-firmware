# Badge Provisioning Tool

Single-page Web Serial tool for programming an attendee's identity onto a badge
at the registration desk. No build step, no dependencies — open `index.html`.

## Running it

Chrome or Edge on desktop (Web Serial is not available in Firefox or Safari,
and not on mobile). Either open the file directly, or serve it:

```
python3 -m http.server 8000 --directory tools/badge-provision
```

The deployed HTTPS version is available at:

<https://hackathon.github.io/badge-firmware/register/>

It is published together with the other badge web tools by
`.github/workflows/deploy-flasher.yml` whenever `main` is deployed, and can
also be deployed from a selected branch with the workflow's manual dispatch.

## Desk procedure

1. Switch the badge **OFF**, plug in a USB-C **data** cable, then switch it on.
2. Click **Connect badge** and choose **USB JTAG/serial debug unit** (often
   shown as Espressif) in the browser prompt.
3. Fill in the form, including the initial app access mode. A four-word badge
   ID is generated for you.
4. Click **Program badge**.
5. Green banner = done. Unplug and hand over **with batteries**.
6. Red banner = **do not hand out**. The tool recovers the connection when it
   can; otherwise it closes the failed port and enables **Connect badge** so the
   operator can retry without manually disconnecting first.

> Don't hold **Start** while plugging in. GPIO9 is the ESP32-C3 boot-mode
> strapping pin, so holding it at reset can drop the badge into serial download
> mode — blank screen, looks broken. Unplug, release, power cycle.

## Existing badges and resets

- **Read badge data** runs `prov show` and loads an already-provisioned
  identity into the form, then runs `appmode` to load its configured app tier.
  It does not add a row to the session log or write anything back to the badge.
- **Clear form only** affects the browser form and never changes the badge.
- **Factory-reset badge** is a confirmed staff action. It first runs
  `prov erase confirm`, then the firmware's `factory_reset confirm`, clearing
  the identity, contacts, settings, and installed Lua apps before rebooting.
  The firmware deliberately preserves `/littlefs/solana.json`, so this action
  preserves an existing Solana wallet key too.

Physical USB disconnects and serial stream errors automatically release the
port and return the controls to a reconnectable state. The tool listens on the
selected `SerialPort`, watches its read stream, and periodically confirms that
the port is still present so browser-specific event delays cannot leave a stale
"Connected" banner behind.

## Keep the CSV

Every successful program is logged with the badge's **BLE MAC**. That mapping is
the only way to re-identify a badge that later loses its identity file, so
download the CSV at the end of each shift and hand it to whoever owns the
attendee database.

The log lives in `localStorage`, so it survives a page reload but is per-browser
and per-machine.

## What it does

Drives the badge's console (`main/hal/hal_console.cpp`):

```
put /littlefs/identity.json <size>   -> READY, then <size> raw bytes, then OK <size>
prov apply                           -> PROV OK id=... mac=...   (or PROV FAIL <reason>)
appmode                            -> APP MODE <mode>
appmode check <admin-password>      -> ADMIN AUTH OK / ADMIN AUTH FAIL
appmode <default|public|all> <admin-password> -> APP MODE OK <mode>
```

The JSON is written straight to its final path — there is no staging file. If
something fails in between, the badge reports itself unprovisioned and shows the
"see registration desk" screen rather than booting with a half-loaded record.
After identity verification, the tool applies one of three permanent firmware
access policies: Default apps, all Public apps, or Public + Internal diagnostic
apps. The chosen mode is included in the local session log and CSV.

### Payloads must be written in chunks

The badge's USB-Serial-JTAG driver has a **256-byte RX ring buffer**
(`USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT`), and `esp_console_new_repl_usb_serial_jtag`
installs that driver itself, so firmware can't enlarge it.

Writing more than ~256 bytes in a single burst overflows the ring before
`cmd_put`'s read loop can drain it. The badge silently loses the tail, then
blocks forever waiting for bytes that were already dropped — which the host
sees as a timeout on `OK <size>`, with no error from the badge.

This tool writes in 128-byte chunks with a 20 ms pause. **Any other tool that
drives `put` needs to do the same.**

## Field limits

Enforced in the form, and they mirror the `char[]` widths in
`main/hal/hal_identity.h`. **Over-length values are rejected, not truncated** —
a silently shortened name is a mangled badge nobody notices until the event.

| Field | Max | | Field | Max |
|---|---|---|---|---|
| `display_name` | 39 | | `net_email` | 63 |
| `account_email` | 63 | | `net_phone` | 19 |
| `claim_id` | 23 | | `net_linkedin` | 39 |
| `badge_id` | 63 | | `net_discord` | 39 |
| | | | `net_instagram` / `net_x` | 31 |

Text is restricted to ASCII plus the Latin-1 supplement. The badge's fonts do
not cover anything else, so a name outside that range would render blank.

> **Note:** the built-in Montserrat fonts are currently ASCII-only. Accented
> characters will not display correctly until the fonts are regenerated with the
> Latin-1 range — see `docs/REMAINING-WORK.md`.

## Badge IDs

Generated in-browser with `crypto.getRandomValues` as four readable,
dash-separated words, for example `otter-falcon-river-willow`. The prototype's
256-word list encodes 32 random bits and remains non-sequential. IDs generated
in the current browser are checked against its local provisioning log. The real
registration tool should enforce uniqueness centrally (or replace this
prototype generator with server-issued IDs) before production use.

Claim IDs are entered and validated as digits in the UI. They remain encoded as
a JSON string on the wire because that is the existing firmware field type.

### Admin password

The **Admin password** is not committed. Enter it in the masked field in both
this dashboard and the engineer flasher; maintainers supply it separately.
The value lasts only for the open page and is never included in identity
JSON, local storage, CSV exports, or the serial log. Provisioning checks it with
`appmode check` before writing identity. Old firmware without the authenticated
command is rejected before programming; flash the current engineer firmware first.

All normal builds include `appmode`, `debug <admin-password>`, and
`seedall <admin-password>`. Reading `appmode` needs no password; changes do.
The firmware stores only a SHA-256 verifier in `main/hal/admin_auth.cpp`.
`tools/test-admin-dashboard.mjs` checks that neither UI commits a default and
that this verifier exists. Rotating the password means updating the verifier.
These commands are an operator guard on the local USB console, not a security
boundary around its other filesystem and provisioning commands.
