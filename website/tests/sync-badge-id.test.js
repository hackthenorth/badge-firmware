import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { BADGE_ID_WORDS, generateBadgeId } from "../src/sync/badge-id.js";

test("badge IDs use the same four-word format and dictionary as registration", () => {
  const source = readFileSync(
    new URL("../../tools/badge-provision/index.html", import.meta.url),
    "utf8"
  );
  const words = JSON.parse(
    source
      .match(/const BADGE_ID_WORDS = (\[[\s\S]*?\]);/)[1]
      .replace(/,\s*]/g, "]")
  );
  assert.deepEqual(BADGE_ID_WORDS, words);
  assert.equal(BADGE_ID_WORDS.length, 256);
  const id = generateBadgeId({
    getRandomValues(bytes) {
      bytes.set([0, 20, 100, 255]);
      return bytes;
    },
  });
  assert.equal(id, [0, 20, 100, 255].map((i) => words[i]).join("-"));
  assert.match(generateBadgeId(), /^[a-z]+(?:-[a-z]+){3}$/);
});
