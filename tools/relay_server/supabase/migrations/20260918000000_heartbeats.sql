-- Heartbeats for the relay and sync-station boxes.
--
-- Each box POSTs to its touch_* RPC every 3 minutes (see heartbeat.c in
-- tools/relay_firmware and tools/sync_station). The first beat from a box
-- creates its row; there is nothing to pre-provision.
--
-- A pg_cron job runs check_heartbeat_staleness() every 10 minutes and POSTs
-- to Slack for any box whose last beat is older than 10 minutes.
--
-- Setup:
--   1. Enable the pg_cron and pg_net extensions (Database -> Extensions),
--      or the create extension lines below do it on push.
--   2. supabase db push   (from tools/relay_server)
--   3. The Slack webhook is already entered in check_heartbeat_staleness();
--      re-push after any change.
--
-- To remove the cron job: select cron.unschedule('heartbeat-watchdog-10min');

create extension if not exists pg_cron;
create extension if not exists pg_net;

-- One table per box type, keyed by box/station id (RELAY_BOX_ID /
-- SYNC_STATION_ID compiled into each box).
create table if not exists public.relay_heartbeats (
  device_id text primary key,
  last_checkin timestamptz not null default now()
);

create table if not exists public.sync_heartbeats (
  device_id text primary key,
  last_checkin timestamptz not null default now()
);

-- Same lockdown as box_decisions: RLS on, no public policies. The boxes
-- cannot touch these tables directly; they go through the touch_* RPCs
-- below, which run as the table owner and bypass RLS.
alter table public.relay_heartbeats enable row level security;
alter table public.sync_heartbeats enable row level security;

-- Beat endpoint for relay boxes. Called as
--   POST /rest/v1/rpc/touch_relay_heartbeat  {"p_device_id": "relay-01"}
-- with the anon key. Server clock stamps the row, so boxes need no time sync.
create or replace function public.touch_relay_heartbeat(p_device_id text)
returns void
language plpgsql
security definer
as $$
begin
  if p_device_id is null or p_device_id = '' then
    return;
  end if;
  insert into public.relay_heartbeats (device_id)
  values (left(p_device_id, 64))
  on conflict (device_id) do update set last_checkin = now();
end;
$$;

create or replace function public.touch_sync_heartbeat(p_device_id text)
returns void
language plpgsql
security definer
as $$
begin
  if p_device_id is null or p_device_id = '' then
    return;
  end if;
  insert into public.sync_heartbeats (device_id)
  values (left(p_device_id, 64))
  on conflict (device_id) do update set last_checkin = now();
end;
$$;

grant execute on function public.touch_relay_heartbeat(text) to anon, authenticated;
grant execute on function public.touch_sync_heartbeat(text) to anon, authenticated;

-- Watchdog: Slack anything stale. Hacky on purpose — the webhook URL is a
-- constant below, no vault. Fires at most once per stale box per 10-minute
-- tick (no cooldown table; a dead box re-alerts every tick until it beats).
create or replace function public.check_heartbeat_staleness()
returns void
language plpgsql
security definer
as $$
declare
  slack_url constant text := 'https://hooks.slack.com/services/REPLACE_ME';
  r record;
  msg text;
begin
  if position('REPLACE_ME' in slack_url) > 0 then
    raise notice 'check_heartbeat_staleness: Slack webhook not configured yet, skipping';
    return;
  end if;

  for r in
    select 'relay' as kind, device_id, last_checkin
      from public.relay_heartbeats
     where last_checkin < now() - interval '10 minutes'
    union all
    select 'sync' as kind, device_id, last_checkin
      from public.sync_heartbeats
     where last_checkin < now() - interval '10 minutes'
  loop
    msg := format(':rotating_light: %s `%s` missed heartbeat (last checkin %s)',
                  r.kind, r.device_id, r.last_checkin);
    perform net.http_post(
      url := slack_url,
      headers := '{"Content-Type": "application/json"}'::jsonb,
      body := jsonb_build_object('text', msg)
    );
  end loop;
end;
$$;

select cron.schedule('heartbeat-watchdog-10min', '*/10 * * * *',
  $$select public.check_heartbeat_staleness()$$);
