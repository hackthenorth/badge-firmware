import assert from "node:assert/strict";
import test from "node:test";
import { loadCloseupPhoto } from "../src/closeup-cache.js";

const imageResponse = (body = "photo pixels") => new Response(body, { headers: { "Content-Type": "image/png" } });

function memoryStorage() {
  const entries = new Map();
  return {
    entries,
    async open() {
      return {
        async match(key) { return entries.get(key)?.clone(); },
        async put(key, response) { entries.set(key, response); },
      };
    },
  };
}

test("revisiting a downloaded photo reads storage without a second network request", async () => {
  const cacheStorage = memoryStorage();
  let requests = 0;
  const fetchPhoto = async () => { requests++; return imageResponse(); };
  const first = await loadCloseupPhoto("/revisit.png", { cacheStorage, fetchPhoto });
  const second = await loadCloseupPhoto("/revisit.png", { cacheStorage, fetchPhoto });
  assert.equal(requests, 1);
  assert.equal(await first.text(), await second.text());
  assert.equal(cacheStorage.entries.size, 1);
});

test("concurrent photo consumers share one download", async () => {
  const cacheStorage = memoryStorage();
  let requests = 0;
  const fetchPhoto = async () => { requests++; return imageResponse(); };
  const [photo, duplicate] = await Promise.all([
    loadCloseupPhoto("/concurrent.png", { cacheStorage, fetchPhoto }),
    loadCloseupPhoto("/concurrent.png", { cacheStorage, fetchPhoto }),
  ]);
  assert.equal(requests, 1);
  assert.equal(photo, duplicate);
});

test("photos load if browser storage is unavailable", async () => {
  const cacheStorage = { async open() { throw new Error("Storage blocked"); } };
  const result = await loadCloseupPhoto("/blocked.png", { cacheStorage, fetchPhoto: async () => imageResponse() });
  assert.equal(await result.text(), "photo pixels");
});

test("photos load if saving to the cache exceeds its quota", async () => {
  const cacheStorage = { async open() { return { async match() {}, async put() { throw new Error("Quota exceeded"); } }; } };
  const result = await loadCloseupPhoto("/quota.png", { cacheStorage, fetchPhoto: async () => imageResponse() });
  assert.equal(await result.text(), "photo pixels");
});

test("failed requests can be retried and are not saved", async () => {
  const cacheStorage = memoryStorage();
  await assert.rejects(loadCloseupPhoto("/retry.png", { cacheStorage, fetchPhoto: async () => new Response("Unavailable", { status: 503 }) }));
  assert.equal(cacheStorage.entries.size, 0);
  const result = await loadCloseupPhoto("/retry.png", { cacheStorage, fetchPhoto: async () => imageResponse() });
  assert.equal(await result.text(), "photo pixels");
});

test("an HTML fallback for a missing image is never cached", async () => {
  const cacheStorage = memoryStorage();
  await assert.rejects(loadCloseupPhoto("/missing.png", {
    cacheStorage,
    fetchPhoto: async () => new Response("<html>app</html>", { headers: { "Content-Type": "text/html" } }),
  }));
  assert.equal(cacheStorage.entries.size, 0);
});

test("a new content hash downloads the replacement instead of the cached original", async () => {
  const cacheStorage = memoryStorage();
  let requests = 0;
  const fetchPhoto = async () => imageResponse(`photo ${++requests}`);
  const oldPhoto = await loadCloseupPhoto("/badge.old.png", { cacheStorage, fetchPhoto });
  const replacement = await loadCloseupPhoto("/badge.new.png", { cacheStorage, fetchPhoto });
  assert.equal(await oldPhoto.text(), "photo 1");
  assert.equal(await replacement.text(), "photo 2");
});
