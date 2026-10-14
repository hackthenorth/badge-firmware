// Supabase persistence for box decisions and per-badge inventories.
//
// Contract: the same (box, badge) pair always resolves to the same stored
// item. The first tap inserts the drawn reward; later taps read that row
// back instead of drawing again. A badge's inventory is derived — every item
// in every row it owns — so it survives cold starts and works across all
// Vercel instances, unlike the old in-memory map.
//
// Talks to PostgREST with plain fetch (Node 20+) so the server gains no new
// npm dependencies. Needs SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY; without
// them the module reports disabled and api/index.js keeps the previous
// in-memory behavior (local dev before the env is wired up).

const BASE = (process.env.SUPABASE_URL ?? "").replace(/\/$/, "");
const KEY = process.env.SUPABASE_SERVICE_ROLE_KEY ?? "";

export const dbEnabled = Boolean(BASE && KEY);

function headers() {
  return {
    apikey: KEY,
    Authorization: `Bearer ${KEY}`,
    "Content-Type": "application/json",
  };
}

// `fetchImpl` is injectable so tests can emulate PostgREST without a network.
async function rest(path, { method = "GET", body, prefer, fetchImpl = fetch } = {}) {
  const res = await fetchImpl(`${BASE}/rest/v1${path}`, {
    method,
    headers: { ...headers(), ...(prefer ? { Prefer: prefer } : {}) },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (!res.ok) {
    const detail = await res.text().catch(() => "");
    throw new Error(`supabase ${method} ${path} -> ${res.status} ${detail}`.trim());
  }
  return res.json();
}

const eq = encodeURIComponent;

export async function loadDecision(box, badge, opts = {}) {
  const rows = await rest(
    `/box_decisions?box=eq.${eq(box)}&badge=eq.${eq(badge)}&select=item&limit=1`,
    opts,
  );
  return rows[0]?.item ?? null;
}

// Insert-or-keep: concurrent first taps race, and the unique (box, badge)
// key is the arbiter. `resolution=ignore-duplicates` turns the loser's
// insert into an empty 201 instead of a 409, so no error handling doubles
// as control flow for the common case.
export async function insertDecision({ box, badge, item, name }, opts = {}) {
  const rows = await rest("/box_decisions", {
    ...opts,
    method: "POST",
    prefer: "resolution=ignore-duplicates,return=representation",
    body: { box, badge, item, name },
  });
  return rows.length > 0 ? "inserted" : "conflict";
}

export async function loadInventory(badge, opts = {}) {
  const rows = await rest(
    `/box_decisions?badge=eq.${eq(badge)}&select=item&order=opened_at`,
    opts,
  );
  return rows.map((r) => r.item);
}

// Full open flow against the database: replay the stored decision when this
// badge has tapped this box before, otherwise draw, store, and keep a
// loser's draw on conflict (re-read the winner's row).
export async function resolveDecision({ box, badge, name }, draw, opts = {}) {
  const existing = await loadDecision(box, badge, opts);
  if (existing !== null) {
    return { item: existing, isNew: false, inventory: await loadInventory(badge, opts) };
  }
  const drawn = draw();
  const outcome = await insertDecision({ box, badge, item: drawn.item, name }, opts);
  if (outcome === "inserted") {
    return { item: drawn.item, isNew: true, inventory: await loadInventory(badge, opts) };
  }
  const winner = await loadDecision(box, badge, opts);
  return { item: winner ?? drawn.item, isNew: false, inventory: await loadInventory(badge, opts) };
}
