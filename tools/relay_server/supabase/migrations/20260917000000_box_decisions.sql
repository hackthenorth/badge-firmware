-- Box decisions: the stored hacker-pal outcome per (station, badge).
--
-- The first tap at a station draws and inserts the row; every later tap from
-- the same badge at the same station reads that row back, so the decision is
-- stable. A badge's inventory is derived, not stored: every item in every row
-- it owns (see the badge_idx index + loadInventory in lib/db.js).
--
-- Access: RLS is on with no public policies. The server talks to PostgREST
-- with the service_role key, which bypasses RLS; the anon key can read and
-- write nothing here.

create table if not exists public.box_decisions (
  box text not null,
  badge text not null,
  item text not null,
  name text not null default '',
  opened_at timestamptz not null default now(),
  primary key (box, badge)
);

create index if not exists box_decisions_badge_idx
  on public.box_decisions (badge);

alter table public.box_decisions enable row level security;
