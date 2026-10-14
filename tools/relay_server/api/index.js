// Solana Scavenger Hunt demo webhook.
//
// The badge builds the request JSON, a BLE relay POSTs it here verbatim, and
// whatever we return goes straight back over BLE to the badge, which parses it
// and puts the item on screen. The relay never looks inside the payload.
//
// Response size matters: it has to fit one BLE advertising frame, so keep the
// body under BBX_MAX_BODY (207) bytes. See main/apps/bbx_proto.h.
//
// Response shape is the quest contract (tools/relay_server/WEBHOOK.md):
//   { new_item, chain_link, all_items }
// all_items is the authoritative inventory — the badge replaces its local
// state with it wholesale. chain_link is the URL behind the badge's B-button
// QR; the production quest server points it at the quest console, the demo
// leaves it empty.

import express from "express";
import { drawReward, rewardFor } from "../lib/rewards.js";
import { record, since, latestId } from "../lib/feed.js";
import { dbEnabled, resolveDecision } from "../lib/db.js";

// Fallback per-attendee inventories, used only when Supabase isn't
// configured (local dev) or is momentarily unreachable. In-memory like the
// feed buffer: a cold start resets it, and the next tap resyncs the badge
// wholesale. With the database up, decisions and inventory live in the
// box_decisions table instead (see supabase/migrations/).
const demoInventories = new Map();

if (!dbEnabled) {
  console.log("supabase not configured (SUPABASE_URL / SUPABASE_SERVICE_ROLE_KEY) — using in-memory decisions");
}

// Artificial latency on /api/open, so the badge's "Opening..." state is
// actually visible instead of flashing past. Well inside both the relay's 8 s
// HTTP timeout (tools/relay_firmware/main/net_post.c) and the badge's 12 s
// give-up (OPEN_TIMEOUT_TICKS). Set to 0 to remove.
const OPEN_DELAY_MS = 3000;

// Demo phone flow behind the badge's QR: the live view, where the attendee
// watches their tap land. The production quest server points this at the
// quest console instead.
const CHAIN_LINK_BASE =
  process.env.CHAIN_LINK_BASE || "https://relay-server-pied.vercel.app/";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

const app = express();

app.use(express.json({ limit: "4kb" }));

// Used by the relay to prove WiFi + DNS + TLS work before any BLE is involved.
app.get("/api/health", (req, res) => {
  res.json({ ok: true, ts: Math.floor(Date.now() / 1000) });
});

// Polled by the live view at /. Pass the last id you saw to get only what's new.
app.get("/api/feed", (req, res) => {
  res.set("Cache-Control", "no-store");
  res.json({ events: since(req.query.since), last: latestId() });
});

// Previous behavior, kept as the no-database fallback: draw fresh every tap
// and accumulate a per-process inventory.
function fallbackDraw(who) {
  const reward = drawReward();
  let owned = demoInventories.get(who);
  if (!owned) {
    owned = [];
    demoInventories.set(who, owned);
  }
  if (!owned.includes(reward.item)) owned.push(reward.item);
  return { reward, owned };
}

app.post("/api/open", async (req, res) => {
  const { box, user_id, name, public_key } = req.body ?? {};

  if (typeof box !== "string" || box.length === 0) {
    return res.status(400).json({ ok: false, error: "missing box" });
  }

  if (OPEN_DELAY_MS > 0) await sleep(OPEN_DELAY_MS);

  // Badge identity: the HTN attendee id, falling back to the wallet.
  const who = String(user_id ?? public_key ?? "anon");
  const displayName = name ?? "";

  // Authoritative inventory for this attendee: the badge adopts it wholesale,
  // so a lost reply resyncs on the next box. Same box + same badge replays
  // the stored decision instead of drawing again.
  let reward;
  let owned;
  if (dbEnabled) {
    try {
      const decision = await resolveDecision({ box, badge: who, name: displayName }, drawReward);
      reward = rewardFor(decision.item) ?? { item: decision.item, name: decision.item, rarity: "common" };
      owned = decision.inventory;
    } catch (err) {
      console.error(`supabase unavailable, falling back to in-memory (${err.message})`);
      ({ reward, owned } = fallbackDraw(who));
    }
  } else {
    ({ reward, owned } = fallbackDraw(who));
  }

  // Vercel's log stream doubles as the event dashboard — one line per tap.
  console.log(
    `open box=${box} user=${user_id ?? "?"} name=${JSON.stringify(name ?? "")} -> ${reward.item} (${reward.name})`,
  );

  record({ box, badge: user_id ?? public_key ?? "", name: name ?? "", item: reward.item, label: reward.name, rarity: reward.rarity });

  res.json({
    new_item: reward.item,
    chain_link: CHAIN_LINK_BASE,
    all_items: owned,
  });
});

// A malformed body from express.json() lands here. Answer in the same shape as
// everything else so the badge has something readable to show.
app.use((err, req, res, next) => {
  if (err?.type === "entity.parse.failed") {
    return res.status(400).json({ ok: false, error: "bad json" });
  }
  console.error(err);
  res.status(500).json({ ok: false, error: "server error" });
});

app.use((req, res) => {
  res.status(404).json({ ok: false, error: "not found" });
});

export default app;
