# Hacker Badge website

Static React/Vite site for `badge.hackthenorth.com`.

The `/closeup` photo viewer uses the original full-resolution PNGs in
`public/closeup/`, rotated clockwise in CSS. The homepage's Ultra Closeup button
opens the selected side with `?side=front` or `?side=back`. The viewer supports
mouse and touch gestures, keyboard controls, minimap navigation, and zoom up to
400% of the original pixel size.

Photos use the browser's persistent Cache Storage after the first view; the
canvas and minimap share a local blob URL. Storage failures fall back to normal
loading. Content hashes in the filenames invalidate both the local cache and
the one-year HTTP cache configured in `public/_headers`; the Vite dev server
applies the same headers to these versioned photos on localhost. When replacing a photo,
update its filename hash and the source in `src/CloseupViewer.jsx`.

Run the viewer checks with `node --test tests/*.test.js` from `website/`.

The partnership section uses the official main Solana logotype from the
[Solana Brand & Press page](https://solana.com/branding), downloaded unchanged
from https://solana.com/src/img/branding/solanaLogo.svg into
`src/assets/solana-logo.svg`.

The battery partnership section uses Jaza's official logo paths, supplied by the
user and verified against [Jaza's website](https://jaza.com/), in
`src/assets/jaza-logo.svg`. Its fill uses Jaza's cyan on a dark blue background.
The mission copy is based on that site's descriptions of solar battery hubs and
Jaza Stars. The badge battery sponsorship credit was supplied by the user.

`src/assets/jaza-batteries.webp` uses the final artwork supplied by the user on
2026-09-14 (`ChatGPT Image Sep 14, 2026, 03_49_58 PM.png`), encoded as WebP at
quality 90 with its original 1536 × 1024 dimensions. This is illustrative artwork,
not a photograph of Jaza's products. Keep replacements at 3:2 and update the image
dimensions in `src/main.jsx` if the resolution changes.

## Local development

```sh
cd website
npm install
npm run dev
```

The production build also copies the existing static badge IDE from
`tools/badgectl/web` into `website/dist/ide`. The tool remains canonical in its
original directory; do not edit the generated copy in `dist`.
The Netlify build-ignore rule includes that source folder and `netlify.toml`,
so IDE-only changes trigger a website deployment even though they are outside
the `website` base directory.

## Netlify

The repository-root `netlify.toml` supplies all build settings:

- base directory: `website`
- build command: `npm run build`
- publish directory: `dist`
- Node.js: `24`
- Functions directory: none

Netlify deploys `/rules` through the SPA fallback and redirects `/ide` to the
copied static IDE at `/ide/`.

The first-party Keep Talking and Nobody Honks decoder manual is available at
`/ktnh`. The badge app encodes this exact route in its ready-room QR code, so
keep the firmware rules and the tables in `src/main.jsx` in sync.

## Badge sync

`/sync` starts with **Connect badge**, then offers two independent paths.
Connecting selects registration for an unregistered badge, or sync for an
already registered badge:

- **Sync & unlock**: read and upload the badge snapshot to Hack the North,
  then unlock public apps. The upload is disclosed before the button. Failed
  uploads keep USB connected and can be retried without reading the badge again.
- **Register / update identity**: read the identity directly over USB, including
  badges that are not registered, then upload a name, badge ID, role, color,
  account email, and optional connection details. This never requires a snapshot,
  server request, or app unlock. `prov apply` loads the new identity without a
  reboot. The command also replays the firmware's registration tutorial.

Badge ID generation uses the same four-word dictionary as the registration
station (for example `acorn-river-moon-fox`). Existing IDs are preserved unless
edited. Email, phone, and social handles have inline validation and examples;
profile URLs are normalized to handles. Account email is account metadata and
is not shared in badge connections; changing it does not create an online account.
There is one **Upload identity to badge** action, with Generate beside the ID
field. Identity JSON download is not part of this flow.

The latest synced snapshot stays in local storage, and identity edits keep a
separate local recovery copy under `htn-badge:identity-backup:v1`. Identity edits
do not replace the original event snapshot or silently upload it. A later
explicit Sync reads and uploads the edited badge. Storage failures are shown.

Downloads appear after a sync read: connections/progress/game/visit CSVs,
per-file downloads, and formatted snapshot JSON. Firmware installation is in a
collapsed section below the workflows; see
[`public/firmware/README.md`](public/firmware/README.md). **Reset your badge**
is a separate dropdown beneath it. Factory reset requires a connected badge
and an explicit checkbox acknowledging deletion. Like the dashboard, it first
sends `prov erase confirm` to remove identity, registration, and the on-badge
sync credential, then `factory_reset confirm` to clear contacts, settings,
app data, and installed Lua apps and restart. It verifies each receipt and
reports partial resets without claiming success. Solana wallet keys and saved
server backups are preserved. Reconnect and use Register / update identity
to set up the badge again; reinstalling firmware is not required.

Open `http://localhost:5173/sync` in desktop Chrome/Edge after `npm run dev`.
Safari is not supported. Close the IDE and other serial clients before connecting.
Sync reads without automatically rebooting; an out-of-memory error asks the
owner to reconnect. The firmware palette is black, warm off-white, and violet.

Unlock and identity writes are enabled in local Vite development. Netlify
explicitly enables both with `VITE_POST_EVENT_UNLOCK=true` and
`VITE_POST_EVENT_IDENTITY=true` in `netlify.toml`; other production builds need
the same flags. Both the UI and USB methods enforce these release switches.
Unlock keeps the synced backup, verifies readback, restarts,
and verifies again. Keep the USB cable connected during writes.

Snapshot uploads use the badge-relay publishable key hardcoded in
`src/sync/supabase.js` (public by design; RLS limits it to inserts), so they
work with zero configuration. `VITE_SUPABASE_URL` and
`VITE_SUPABASE_ANON_KEY` at build time (Netlify env vars in production,
`website/.env` locally — see `.env.example`) override the defaults.
The Supabase schema, setup, and append-only upload contract are in
[`supabase/README.md`](supabase/README.md).

Run all website checks with `npm test`. Only the website build is needed for
this change; no firmware build or flash is required.
