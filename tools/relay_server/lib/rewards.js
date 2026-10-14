// Demo reward pool for the Solana Scavenger Hunt test server.
//
// Item ids are the contract: they map to pal slots through the badge's
// compile-time pal catalog (main/apps/pal_catalog.cpp), so dealing real ids
// exercises the actual unlock path instead of rendering placeholders.
// Keep the ids short — the whole JSON response has to survive a trip back
// through a single BLE advertising frame, which leaves BBX_MAX_BODY (207)
// bytes for the body (see main/apps/bbx_proto.h).
export const REWARDS = [
  { item: "1", name: "Ginny - Locked In", rarity: "common" },
  { item: "2", name: "Ginny - Sparkle",   rarity: "uncommon" },
  { item: "3", name: "Patch - Snooze",    rarity: "common" },
  { item: "4", name: "Patch - Sparkle",   rarity: "rare" },
  { item: "5", name: "Vinyl - Curious",   rarity: "common" },
  { item: "6", name: "Ginny - Heart",     rarity: "uncommon" },
  { item: "7", name: "Patch - Curious",   rarity: "uncommon" },
  { item: "8", name: "Vinyl - Snooze",    rarity: "rare" },
  { item: "9", name: "Vinyl - Sparkle",   rarity: "rare" },
];

// Rare things should actually be rare, so weight the draw instead of picking
// uniformly across the list.
const WEIGHTS = { common: 5, uncommon: 3, rare: 1 };

// Look up a previously stored decision in the catalog. Returns undefined
// when the id is unknown (e.g. the catalog changed since the draw) — the
// caller still honors the stored id, it just lacks a display name.
export function rewardFor(item) {
  return REWARDS.find((r) => r.item === item);
}

export function drawReward() {
  const total = REWARDS.reduce((sum, r) => sum + WEIGHTS[r.rarity], 0);
  let roll = Math.random() * total;
  for (const r of REWARDS) {
    roll -= WEIGHTS[r.rarity];
    if (roll <= 0) return r;
  }
  return REWARDS[0];
}
