const CACHE_NAME = "badge-closeup-images-v1";
const pending = new Map();

async function readPhoto(src, cacheStorage, fetchPhoto) {
  let cache;
  try {
    cache = await cacheStorage?.open(CACHE_NAME);
    const cached = await cache?.match(src);
    if (cached?.ok) return await cached.blob();
  } catch {
    // Browser storage can be disabled or full; the photo can still load normally.
  }

  const response = await fetchPhoto(src, { cache: "force-cache" });
  if (!response.ok || !response.headers.get("content-type")?.startsWith("image/")) {
    throw new Error("Could not load the closeup photo");
  }
  try {
    await cache?.put(src, response.clone());
  } catch {
    // A failed cache write must not prevent viewing the downloaded photo.
  }
  return response.blob();
}

// Share in-flight requests, including React StrictMode's repeated effect setup.
// Only the persistent cache retains completed downloads.
export function loadCloseupPhoto(src, { cacheStorage = globalThis.caches, fetchPhoto = globalThis.fetch } = {}) {
  if (!pending.has(src)) {
    pending.set(src, readPhoto(src, cacheStorage, fetchPhoto).finally(() => pending.delete(src)));
  }
  return pending.get(src);
}
