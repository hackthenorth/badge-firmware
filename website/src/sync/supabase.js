// Append-only badge snapshot uploads to the badge-relay Supabase project.
// Browsers POST the original fs_mirror document with the publishable key;
// Row Level Security permits INSERT of the `dump` column only, so there is
// no login, token, or returned row. `env` is injectable so node tests can
// prove the request shape without a network or Vite.

export const DEFAULT_SUPABASE_URL = "https://qoouxoeavyuokuscvxff.supabase.co";
// Publishable key for the badge-relay project. Public by design (it already
// ships in every badge firmware binary): RLS limits it to INSERT on
// badge_dumps, so hardcoding it lets uploads work with zero configuration.
// A build-time VITE_SUPABASE_ANON_KEY still overrides it.
export const DEFAULT_SUPABASE_ANON_KEY =
  "sb_publishable_3z2RXbH115gatxfvS3xDAA_SrfVpzJs";

export function supabaseUrl(env = (import.meta.env ?? {})) {
  return String(env.VITE_SUPABASE_URL ?? DEFAULT_SUPABASE_URL).replace(
    /\/+$/,
    ""
  );
}

export function supabaseKey(env = (import.meta.env ?? {})) {
  return env.VITE_SUPABASE_ANON_KEY ?? DEFAULT_SUPABASE_ANON_KEY;
}

export function supabaseConfigured(env = (import.meta.env ?? {})) {
  return Boolean(supabaseKey(env));
}

// Only snapshots read from a real badge may be uploaded. Demo and imported
// JSON stay local no matter how deliberately the button is clicked.
export function isUploadableSource(source) {
  return typeof source === "string" && source.startsWith("Haker Badge");
}

export async function uploadSnapshot(
  doc,
  { env = (import.meta.env ?? {}), fetchImpl = fetch } = {}
) {
  const key = supabaseKey(env);
  if (!key) throw new Error("Snapshot uploads are not configured in this build.");
  // Publishable key travels in the `apikey` header only: that works for both
  // legacy anon JWTs and new sb_publishable_ keys, while a Bearer token is
  // only valid for JWTs. RLS still sees the request as anon either way.
  const res = await fetchImpl(`${supabaseUrl(env)}/rest/v1/badge_dumps`, {
    method: "POST",
    signal: AbortSignal.timeout(30000),
    headers: {
      apikey: key,
      "Content-Type": "application/json",
      Prefer: "return=minimal",
    },
    body: JSON.stringify({ dump: doc }),
  });
  if (!res.ok) {
    const detail = await res.text().catch(() => "");
    throw new Error(
      `Upload failed (${res.status}${detail ? `: ${detail.slice(0, 160)}` : ""}). Your local snapshot is unchanged. Try again.`
    );
  }
}
