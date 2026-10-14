#pragma once

// Spawns the background Supabase heartbeat task: every 3 minutes it calls
// the touch_relay_heartbeat() RPC (see tools/relay_server/supabase/
// migrations/*_heartbeats.sql) keyed by CONFIG_RELAY_BOX_ID. A pg_cron
// watchdog Slacks when a box goes stale. If the SUPABASE_URL/ANON_KEY
// config is empty the task logs once and exits.
void heartbeat_start(void);
