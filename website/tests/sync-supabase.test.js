import test from "node:test";
import assert from "node:assert/strict";
import {
  DEFAULT_SUPABASE_ANON_KEY,
  DEFAULT_SUPABASE_URL,
  isUploadableSource,
  supabaseConfigured,
  supabaseUrl,
  uploadSnapshot,
} from "../src/sync/supabase.js";
import { demoSnapshot } from "../src/sync/demo.js";

const ENV = {
  VITE_SUPABASE_URL: "https://qoouxoeavyuokuscvxff.supabase.co/",
  VITE_SUPABASE_ANON_KEY: "test-publishable-key",
};

function captureFetch(status = 201, text = "") {
  const calls = [];
  const fetchImpl = async (url, init) => {
    calls.push({ url, init });
    return { ok: status >= 200 && status < 300, status, text: async () => text };
  };
  return { calls, fetchImpl };
}

test("posts the original dump to badge_dumps with minimal return and the publishable key", async () => {
  const { calls, fetchImpl } = captureFetch();
  await uploadSnapshot(demoSnapshot, { env: ENV, fetchImpl });
  assert.equal(calls.length, 1);
  const [{ url, init }] = calls;
  assert.equal(url, `${DEFAULT_SUPABASE_URL}/rest/v1/badge_dumps`);
  assert.equal(init.method, "POST");
  assert.ok(init.signal instanceof AbortSignal);
  assert.equal(init.signal.aborted, false);
  assert.equal(init.headers.apikey, "test-publishable-key");
  assert.ok(!("Authorization" in init.headers));
  assert.equal(init.headers["Content-Type"], "application/json");
  assert.equal(init.headers.Prefer, "return=minimal");
  assert.deepEqual(JSON.parse(init.body), { dump: demoSnapshot });
});

test("a custom project URL is honored and trailing slashes are trimmed", async () => {
  const { calls, fetchImpl } = captureFetch();
  await uploadSnapshot(demoSnapshot, {
    env: { ...ENV, VITE_SUPABASE_URL: "https://example.supabase.co///" },
    fetchImpl,
  });
  assert.ok(calls[0].url.startsWith("https://example.supabase.co/rest/v1/"));
  assert.equal(supabaseUrl({}), DEFAULT_SUPABASE_URL);
});

test("a failed upload throws with the status and never claims success", async () => {
  const { fetchImpl } = captureFetch(401, "Invalid API key");
  await assert.rejects(
    uploadSnapshot(demoSnapshot, { env: ENV, fetchImpl }),
    /Upload failed \(401: Invalid API key\)/
  );
});

test("an explicitly blank key fails before any request", async () => {
  const { calls, fetchImpl } = captureFetch();
  await assert.rejects(
    uploadSnapshot(demoSnapshot, {
      env: { ...ENV, VITE_SUPABASE_ANON_KEY: "" },
      fetchImpl,
    }),
    /not configured/
  );
  assert.equal(calls.length, 0);
  assert.equal(supabaseConfigured({ VITE_SUPABASE_ANON_KEY: "" }), false);
});

test("the hardcoded publishable key is the default", async () => {
  assert.match(DEFAULT_SUPABASE_ANON_KEY, /^sb_publishable_/);
  assert.equal(supabaseConfigured({}), true);
  assert.equal(supabaseConfigured(ENV), true);
  const { calls, fetchImpl } = captureFetch();
  await uploadSnapshot(demoSnapshot, { env: {}, fetchImpl });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].init.headers.apikey, DEFAULT_SUPABASE_ANON_KEY);
});

test("only snapshots read from a real badge are uploadable", () => {
  assert.equal(isUploadableSource("Haker Badge"), true);
  assert.equal(isUploadableSource("Haker Badge · before unlock"), true);
  assert.equal(isUploadableSource("Haker Badge · apps unlocked"), true);
  assert.equal(isUploadableSource("Demo · fictional data"), false);
  assert.equal(isUploadableSource("Imported JSON"), false);
  assert.equal(isUploadableSource(""), false);
  assert.equal(isUploadableSource(null), false);
});
