import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
  buildIdentity,
  IDENTITY_FIELDS,
  IDENTITY_ROLES,
  identityDraft,
  readIdentity,
  identityFieldError,
  normalizeIdentityField,
} from "../src/sync/identity.js";
import { releaseFeatures } from "../src/sync/features.js";
import { demoSnapshot } from "../src/sync/demo.js";

const validDraft = () => ({
  ...identityDraft(),
  display_name: "Alex",
  badge_id: "acorn-river-moon-fox",
});
test("release flags require an explicit production opt-in for each action", () => {
  assert.deepEqual(releaseFeatures(), { unlock: false, identity: false });
  assert.deepEqual(releaseFeatures({ DEV: true }), {
    unlock: true,
    identity: true,
  });
  assert.deepEqual(releaseFeatures({ VITE_POST_EVENT_UNLOCK: "true" }), {
    unlock: true,
    identity: false,
  });
  assert.deepEqual(releaseFeatures({ VITE_POST_EVENT_IDENTITY: "true" }), {
    unlock: false,
    identity: true,
  });
  assert.deepEqual(
    releaseFeatures({
      VITE_POST_EVENT_UNLOCK: "1",
      VITE_POST_EVENT_IDENTITY: "yes",
    }),
    { unlock: false, identity: false }
  );
});
test("editor role names and byte limits match the firmware contract", () => {
  const header = readFileSync(
    new URL("../../main/hal/hal_identity.h", import.meta.url),
    "utf8"
  );
  for (const { key, max } of IDENTITY_FIELDS)
    assert.match(header, new RegExp(`char\\s+${key}\\[${max + 1}\\]`));
  const source = readFileSync(
    new URL("../../main/hal/hal_identity.cpp", import.meta.url),
    "utf8"
  );
  const keys = source.match(/kRoleKeys\[ROLE_COUNT\] = \{([\s\S]*?)\};/)[1];
  assert.deepEqual(
    [...keys.matchAll(/"([^"]+)"/g)].map((m) => m[1]),
    IDENTITY_ROLES.map(([key]) => key)
  );
});
test("edits preserve registration metadata and unknown fields while allowing role/color changes", () => {
  const base = {
    ...validDraft(),
    attendee_id: 123,
    claim_id: "42",
    provisioned_unix: 456,
    future_field: "keep",
  };
  const identity = buildIdentity(base, {
    ...identityDraft(base),
    display_name: "Grace",
    role: "sponsor",
    role_color: "#123abc",
  });
  assert.equal(identity.claim_id, "42");
  assert.equal(identity.attendee_id, 123);
  assert.equal(identity.provisioned_unix, 456);
  assert.equal(identity.future_field, "keep");
  assert.equal(identity.display_name, "Grace");
  assert.equal(identity.role, "sponsor");
  assert.equal(identity.role_color, "#123abc");
  assert.ok(
    !(
      "role_color" in
      buildIdentity(identity, { ...identityDraft(identity), role_color: "" })
    )
  );
});
test("identity validation rejects control characters, overlong UTF-8, invalid roles and IDs", () => {
  for (const draft of [
    { ...validDraft(), display_name: "é".repeat(20) },
    { ...validDraft(), display_name: "bad\nname" },
    { ...validDraft(), display_name: "" },
    { ...validDraft(), role: "superuser" },
    { ...validDraft(), badge_id: "../identity" },
    { ...validDraft(), role_color: "#zzz000" },
  ])
    assert.throws(() => buildIdentity({}, draft));
  assert.equal(
    buildIdentity({}, { ...validDraft(), display_name: "Zoë" }).display_name,
    "Zoë"
  );
});
test("missing identities are only treated as fresh on a complete unprovisioned snapshot", () => {
  const doc = structuredClone(demoSnapshot);
  delete doc.files["identity.json"];
  assert.throws(() => readIdentity(doc), /incomplete/);
  doc.provisioned = false;
  assert.deepEqual(readIdentity(doc), {});
  doc.complete = false;
  assert.throws(() => readIdentity(doc), /incomplete/);
});
test("legacy numeric roles and RGB colors populate the editor correctly", () => {
  const draft = identityDraft({ role: 4, role_color: [255, 128, 0] });
  assert.equal(draft.role, "mentor");
  assert.equal(draft.role_color, "#ff8000");
});

test("contact validation accepts common formats and rejects malformed values with hints", () => {
  const valid = {
    account_email: "alex@example.com",
    net_email: "alex+badge@example.co.uk",
    net_phone: "+1 (519) 555-0123",
    net_linkedin: "https://www.linkedin.com/in/alex-hacker/",
    net_instagram: "@alex.hacker",
    net_discord: "alex.hacker",
    net_x: "https://x.com/alex_hacker",
  };
  for (const [key, value] of Object.entries(valid))
    assert.equal(identityFieldError(key, value), "", key);
  assert.equal(identityFieldError("net_discord", "Alex#1234"), "");
  const invalid = {
    account_email: "not-an-email",
    net_email: "alex@",
    net_phone: "call me tomorrow",
    net_linkedin: "https://evil.example/alex",
    net_instagram: "two words",
    net_discord: "a..b",
    net_x: "a".repeat(16),
  };
  for (const [key, value] of Object.entries(invalid))
    assert.ok(identityFieldError(key, value), key);
  for (const key of Object.keys(valid))
    assert.equal(identityFieldError(key, ""), "", "contacts are optional");
  assert.ok(identityFieldError("net_phone", "123"));
  assert.equal(
    normalizeIdentityField("net_linkedin", valid.net_linkedin),
    "alex-hacker"
  );
  const identity = buildIdentity({}, { ...validDraft(), ...valid });
  assert.equal(identity.net_instagram, "alex.hacker");
  assert.equal(identity.net_linkedin, "alex-hacker");
  assert.equal(identity.net_x, "alex_hacker");
});

test("new IDs use four words but an existing legacy ID can be kept", () => {
  assert.ok(identityFieldError("badge_id", "badge-12345"));
  assert.equal(
    identityFieldError("badge_id", "badge-12345", { badge_id: "badge-12345" }),
    ""
  );
  assert.equal(identityFieldError("badge_id", "acorn-river-moon-fox"), "");
});
