# Solana Scavenger Hunt demo server

The third piece of the Solana Scavenger Hunt flow:

```
badge app  --BLE(BBX1)-->  relay firmware  --HTTPS POST-->  this server
badge app  <-BLE(BBX1)---  relay firmware  <---HTTP 200---  this server
```

It's a stand-in for whatever eventually decides what's in a box. Right now it
deals a random pal reward id (`lib/rewards.js`), so taps exercise the badge's
real unlock path instead of rendering placeholders.

**[WEBHOOK.md](WEBHOOK.md) is the full spec**: field semantics, size limits,
error behaviour, and a JSON Schema. Read that if you're replacing this server
with a real one. The summary below is enough to test against.

## Contract

### `POST /api/open`

The badge builds this body; the relay forwards the bytes untouched.

```json
{ "box": "orchid-01", "user_id": "123456", "name": "Nick", "email": "n@x.com", "public_key": "5oNDL3..." }
```

Response:

```json
{ "new_item": "3", "chain_link": "https://relay-server-pied.vercel.app/", "all_items": ["3"] }
```

The badge reads `new_item`, `chain_link`, and `all_items` (authoritative
inventory, replaced wholesale, not merged).

**Keep responses under 207 bytes (BBX_MAX_BODY).** They travel back inside a
single BLE advertising frame. Anything longer is truncated by the relay, and
the badge shows "Empty box?" instead of an item. See `main/apps/bbx_proto.h`.

### `GET /api/health`

```json
{ "ok": true, "ts": 1785000000 }
```

Point the relay at this first: it proves WiFi, DNS, and TLS work before any
BLE is in the picture.

## Run locally

```bash
cd tools/relay_server
npm install
npx vercel dev
```

Then:

```bash
curl -s localhost:3000/api/health

curl -s -X POST localhost:3000/api/open \
  -H 'Content-Type: application/json' \
  -d '{"box":"orchid-01","user_id":"123456","name":"Nick"}'
```

Check the response actually fits the radio budget:

```bash
curl -s -X POST localhost:3000/api/open \
  -H 'Content-Type: application/json' \
  -d '{"box":"orchid-01","user_id":"123456","name":"Nick"}' | wc -c
# want: comfortably under 207
```

## Deploy

```bash
npx vercel --prod
```

The resulting URL (plus `/api/open`) is what goes into the relay's
`RELAY_WEBHOOK_URL` (see `tools/relay_firmware/README.md`).

There is no authentication on this endpoint. That's a deliberate choice for the
proof of concept; anyone who finds the URL can draw fruit from it.

## Persistent decisions (Supabase)

The first tap from a badge at a station draws a reward and stores it in the
Supabase `box_decisions` table; every later tap from the same badge at the
same station replays that stored item instead of drawing again. A badge's
inventory is derived from every row it owns, so it survives cold starts and
is shared across all serverless instances. Badge identity is the HTN
attendee id (`user_id`), falling back to the wallet (`public_key`).

Without `SUPABASE_URL` / `SUPABASE_SERVICE_ROLE_KEY` the server keeps the old
behavior (random draw + in-memory inventory) and logs a line saying so; if
the database is momentarily unreachable it also falls back per-request
instead of failing the tap. The live feed at `/` stays in-memory either way.

### One-time setup

The database is the `badge-relay` Supabase project, linked from this
directory (`supabase/` holds the link + migrations).

1. Apply the schema. The DB password was generated at project creation and
   isn't stored anywhere. Reset it first under
   [Database Settings](https://supabase.com/dashboard/project/qoouxoeavyuokuscvxff/settings/database),
   then run (it prompts for the password):

   ```bash
   cd tools/relay_server
   supabase link --project-ref qoouxoeavyuokuscvxff
   supabase db push
   ```

2. Get the API keys (`Project Settings → API`, or
   `supabase projects api-keys --project-ref qoouxoeavyuokuscvxff`)
   and set them on the Vercel project (each prompts for the value):

   ```bash
   npx vercel env add SUPABASE_URL production
   npx vercel env add SUPABASE_SERVICE_ROLE_KEY production
   ```

   `SUPABASE_URL` is `https://qoouxoeavyuokuscvxff.supabase.co`.
   Use the `service_role` key: it stays server-side and bypasses RLS
   (the table has RLS on with no public policies, so the `anon` key
   can neither read nor write it).

3. Redeploy: `npx vercel --prod`.

## Box heartbeats

The relay and sync-station boxes prove they're alive by POSTing their id to
the `touch_relay_heartbeat()` / `touch_sync_heartbeat()` RPCs every 3 minutes
(`heartbeat.c` in each firmware project, keyed by box/station id). Schema plus
a pg_cron job that Slacks for any box stale over 10 minutes live in
`supabase/migrations/20260918000000_heartbeats.sql` (pushed with the same
`supabase db push` as above (enable `pg_cron` + `pg_net` under Database →
Extensions first).

Set the Slack webhook in `check_heartbeat_staleness()`; it ships as a
`REPLACE_ME` placeholder, which makes the watchdog skip alerting. The other
remaining value is the Supabase anon key in each box's menuconfig
(`RELAY/SYNC_SUPABASE_ANON_KEY`). Alerts start after a box's first beat
creates its row. A box that never checked in has no row to go stale.

## Post-event badge snapshots

`supabase/migrations/20260920000000_badge_dumps.sql` adds the append-only
`badge_dumps` table collected from `/sync` on badge.hackthenorth.com: one
row per uploaded badge snapshot (`dump_id`, server-stamped `dumped_at`, and
the original `dump` JSON). It ships with the same `supabase db push` as
above. Browsers INSERT with the publishable key under an insert-only RLS
policy; there are no public read/update/delete grants, so organizers read
rows via the dashboard or a server-side key. The browser contract and the
website env vars (`VITE_SUPABASE_URL`, `VITE_SUPABASE_ANON_KEY`) are
documented in `../../website/supabase/README.md`.

To verify against the live database after deploying, tap the same station
twice with the same badge and check both replies carry the same `new_item`:

```bash
for i in 1 2; do curl -s -X POST https://<your-app>.vercel.app/api/open \
  -H 'Content-Type: application/json' \
  -d '{"box":"orchid-01","user_id":"123456","name":"Nick"}'; echo; done
```
