import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import {
  contactsCsv,
  csv,
  describeSnapshot,
  fields,
  parseSnapshot,
  planUnlock,
  progressCsv,
  PUBLIC_APPS,
  recordsCsv,
  textFile,
  visitsCsv,
} from "../src/sync/data.js";
import { demoSnapshot } from "../src/sync/demo.js";
import { parseSnapshotReply } from "../src/sync/serial.js";
const clone = () => structuredClone(demoSnapshot);

test("public unlock list matches the active firmware registry, excluding internal apps", () => {
  const main = readFileSync(
    new URL("../../main/main.cpp", import.meta.url),
    "utf8"
  ).replace(/\/\/[^\n]*/g, "");
  const registered = [
    ...main.matchAll(/app_registry_add\((app_\w+_instance)\(\)\)/g),
  ].map((m) => m[1]);
  const publicSlugs = readdirSync(new URL("../../main/apps/", import.meta.url))
    .filter((name) => name.endsWith(".cpp"))
    .flatMap((name) => {
      const source = readFileSync(
        new URL(`../../main/apps/${name}`, import.meta.url),
        "utf8"
      );
      if (!registered.some((factory) => source.includes(`${factory}()`)))
        return [];
      if (
        /AppAccess access\(\) const override\s*\{\s*return AppAccess::(Default|Internal)/.test(
          source
        )
      )
        return [];
      const literal =
        /const char \*slug\(\) const override\s*\{\s*return "([^"]+)"/.exec(
          source
        );
      const constant = /const char \*const kSlug = "([^"]+)"/.exec(source);
      assert.ok(literal || constant, `Find public slug in ${name}`);
      return [(literal || constant)[1]];
    });
  assert.deepEqual([...PUBLIC_APPS].sort(), publicSlugs.sort());
});

test("snapshot and serial parsing preserve unknown data and escaped prompt text", () => {
  const doc = clone();
  doc.files["appdata/new.cfg"] = { text: "prompt=badge> \n🦆" };
  const raw = JSON.stringify(doc);
  assert.deepEqual(parseSnapshot(raw), doc);
  assert.deepEqual(
    parseSnapshotReply(
      `snapshot\r\nI (123) tag: noise\r\n${raw}\r\nbytes=900\r\nbadge> `
    ),
    doc
  );
  assert.throws(() => parseSnapshot("{}"), /filesystem snapshot/);
  assert.throws(() => parseSnapshot("["), /valid JSON/);
  assert.throws(() => parseSnapshot("x".repeat(65537)), /too large/);
  assert.throws(
    () => parseSnapshotReply("no room for a snapshot"),
    /out of memory/
  );
});

test("summaries decode contact details, progress and game records like the dashboard", () => {
  const result = describeSnapshot(clone());
  assert.equal(result.contacts.length, 2);
  assert.equal(result.contacts[0].name, "Ada Example");
  assert.equal(
    result.contacts[0].details.find((d) => d.label === "LinkedIn").href,
    "https://www.linkedin.com/in/ada-example"
  );
  assert.equal(
    result.summary.games.find((g) => g.name === "Snake").metrics[0].value,
    1280
  );
  assert.equal(
    result.summary.progress.find((p) => p.label === "Solana scavenger hunt")
      .count,
    5
  );
  assert.equal(result.visits.length, 2);
  assert.equal(result.partial, false); // a binary skip is an expected omission
  assert.equal(result.issues.length, 1);
  assert.match(contactsCsv(result.contacts), /ada@example.com/);
  assert.match(recordsCsv(result.summary), /1280/);
  assert.match(progressCsv(result.summary), /Apps discovered/);
  assert.match(visitsCsv(result.visits), /Demo partner/);
});

test("partial and malformed files are visible, while ambiguous settings cannot be written", () => {
  const doc = clone();
  doc.files["config/system.cfg"].trunc = true;
  doc.files["config/contacts/demo-ada.cfg"].trunc = true;
  assert.equal(describeSnapshot(doc).partial, true);
  assert.equal(describeSnapshot(doc).contacts.length, 1);
  assert.throws(() => planUnlock(doc), /incomplete/);
  delete doc.files["config/system.cfg"].trunc;
  doc.files["/littlefs/config/system.cfg"] = doc.files["config/system.cfg"];
  assert.equal(textFile(doc, "config/system.cfg"), null);
  assert.throws(() => planUnlock(doc), /incomplete/);
  delete doc.files["/littlefs/config/system.cfg"];
  doc.files["config/system.cfg"].text += "launcher_order=snake\n";
  assert.throws(() => planUnlock(doc), /ambiguous/);
});

test("unlock appends public apps, preserves custom order/settings, and is idempotent", () => {
  const doc = clone();
  doc.files["config/system.cfg"].text +=
    "brightness=31\ncustom_setting=keep=me\n";
  const before = fields(doc.files["config/system.cfg"].text);
  const plan = planUnlock(doc);
  assert.ok(plan.added.length > 0);
  assert.equal(fields(plan.updated).custom_setting, "keep=me");
  assert.equal(fields(plan.updated).brightness, "31");
  assert.ok(
    fields(plan.updated).launcher_order.startsWith(before.launcher_order + ",")
  );
  assert.ok(
    !plan.added.includes("diagnostics") && !plan.added.includes("flashlight")
  );
  assert.ok(
    PUBLIC_APPS.every((slug) =>
      fields(plan.updated).launcher_order.split(",").includes(slug)
    )
  );
  assert.equal(doc.files["config/system.cfg"].text, plan.original);
  doc.files["config/system.cfg"].text = plan.updated;
  assert.deepEqual(planUnlock(doc).added, []);
});

test("unlock refuses oversized launcher order before modifying a badge", () => {
  const doc = clone();
  doc.files["config/system.cfg"].text =
    "launcher_order=" + "a".repeat(1000) + "\n";
  assert.throws(() => planUnlock(doc), /launcher is full/);
});

test("CSV quotes commas, Unicode and line breaks and neutralizes formulas", () => {
  assert.equal(
    csv([
      ["Name", "Value"],
      ['Ada, "É"', '=HYPERLINK("evil")'],
      ["new\nline", " +12"],
      ["@run", "\tcalc"],
    ]),
    '\uFEFF"Name","Value"\r\n"Ada, ""É""","\'=HYPERLINK(""evil"")"\r\n"new\nline","\' +12"\r\n"\'@run","\'\tcalc"\r\n'
  );
});

test("contact display and CSV use readable email and phone values without unreliable dates", () => {
  const doc = clone();
  doc.files["config/contacts/demo-ada.cfg"].text =
    "badge_id=demo-ada\nname=Ada Example\nrole=0\nemail=mailto:ada%40example.com\nphone=tel:%2B1%20(555)%20123-4567\nreceived_unix=60\n";
  const { contacts } = describeSnapshot(doc);
  const email = contacts[0].details.find((detail) => detail.label === "Email");
  const phone = contacts[0].details.find((detail) => detail.label === "Phone");
  assert.equal(email.value, "ada@example.com");
  assert.equal(phone.value, "+1 (555) 123-4567");
  assert.equal(email.href, "mailto:ada%40example.com");
  assert.equal(phone.href, "tel:+15551234567");
  const exported = contactsCsv(contacts);
  assert.match(exported, /ada@example.com/);
  assert.match(exported, /\+1 \(555\) 123-4567/);
  assert.doesNotMatch(
    exported,
    /mailto:|tel:|%40|Email link|Phone link|Connected at|1970/
  );
  assert.ok(
    doc.files["config/contacts/demo-ada.cfg"].text.includes("received_unix=60")
  );
});

test("malformed imported file entries cannot become React elements or markup", () => {
  for (const entry of [
    null,
    [],
    { text: {} },
    { size: {} },
    { size: -1 },
    { skip: {} },
    { trunc: "yes" },
  ]) {
    const doc = clone();
    doc.files["malformed"] = entry;
    assert.throws(
      () => parseSnapshot(JSON.stringify(doc)),
      /invalid file entry/
    );
  }
  const doc = clone();
  doc.files["unknown.txt"] = {
    text: "<script>no execution</script>",
    custom: { future: true },
  };
  assert.deepEqual(parseSnapshot(JSON.stringify(doc)), doc);
});
