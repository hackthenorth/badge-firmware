import test from "node:test";
import assert from "node:assert/strict";
import { serialBuild } from "./helpers/serial-build.js";
import { identityDraft } from "../src/sync/identity.js";
const { BadgeSerial } = await serialBuild({ DEV: true });
import { demoSnapshot } from "../src/sync/demo.js";
import { PUBLIC_APPS, fields } from "../src/sync/data.js";

// Real Web Streams, a fragmented USB reply, and a small RX ring. This exercises
// the transport without connecting to or mutating an attendee's physical badge.
class FakePort extends EventTarget {
  constructor({
    badReceipt = false,
    silent = false,
    badReadback = false,
    badBoot = false,
    badUnlockBoot = false,
    partialReset = false,
    eraseDenied = false,
    eraseUnsupported = false,
    identityReadError = null,
    missingFileDiagnostic = "ERROR",
  } = {}) {
    super();
    this.doc = structuredClone(demoSnapshot);
    this.chunks = [];
    this.commands = [];
    this.badReceipt = badReceipt;
    this.badReadback = badReadback;
    this.badBoot = badBoot;
    this.partialReset = partialReset;
    this.eraseDenied = eraseDenied;
    this.eraseUnsupported = eraseUnsupported;
    this.identityReadError = identityReadError;
    this.missingFileDiagnostic = missingFileDiagnostic;
    this.hasSyncCredential = true;
    this.badUnlockBoot = badUnlockBoot;
    this.silent = silent;
    this.closed = false;
  }
  async open(options) {
    assert.equal(options.baudRate, 115200);
    this.closed = false;
    this.readable = new ReadableStream({
      start: (controller) => {
        this.output = controller;
      },
    });
    this.writable = new WritableStream({ write: (bytes) => this.write(bytes) });
  }
  async close() {
    this.closed = true;
  }
  reply(text) {
    if (this.silent) return;
    const bytes = new TextEncoder().encode(text);
    // Split in the middle of prompt tokens and multi-byte Unicode.
    for (let i = 0; i < bytes.length; i += 17)
      this.output.enqueue(bytes.slice(i, i + 17));
  }
  write(bytes) {
    if (this.pending) {
      assert.ok(bytes.length <= 128, "payload fits the hardware RX ring");
      this.chunks.push(bytes);
      this.pending.received.push(...bytes);
      if (this.pending.received.length === this.pending.size) {
        this.doc.files[this.pending.path] = {
          text: this.badReadback
            ? "invalid"
            : new TextDecoder().decode(new Uint8Array(this.pending.received)),
        };
        const size = this.pending.size;
        this.pending = null;
        this.reply(
          `${this.badReceipt ? "write error" : `OK ${size}`}\r\nbadge> `
        );
      }
      return;
    }
    if (bytes.length === 1 && bytes[0] === 3) return;
    const line = new TextDecoder().decode(bytes);
    assert.ok(
      line.endsWith("\r") && !line.endsWith("\r\n"),
      "commands end in bare CR"
    );
    assert.ok(bytes.length < 256);
    const command = line.slice(0, -1);
    this.commands.push(command);
    if (!command) this.reply("\r\nbadge> ");
    else if (command === "reboot") {
      if (this.badUnlockBoot && this.chunks.length)
        this.doc.files["config/system.cfg"].text = "launcher_order=badgeid\n";
      const identity = this.doc.files["identity.json"];
      if (identity && !this.badBoot) {
        this.doc.badge_id = JSON.parse(identity.text).badge_id;
        this.doc.provisioned = true;
      }
      this.reply("reboot\r\nrebooting...\r\nbooting\r\nbadge> ");
    } else if (command === "prov show") {
      this.reply(
        `${command}\r\nprovisioned=${
          this.doc.provisioned ? 1 : 0
        }\r\nbadge_id=${this.doc.badge_id}\r\nbadge> `
      );
    } else if (command === "cat /littlefs/identity.json") {
      const failed = this.identityReadError || !this.doc.files["identity.json"];
      const body = failed
        ? `open /littlefs/identity.json: ${
            this.identityReadError || "No such file or directory"
          }${
            this.missingFileDiagnostic
              ? `\r\nCommand returned non-zero error code: 0x1 (${this.missingFileDiagnostic})`
              : ""
          }`
        : this.doc.files["identity.json"].text;
      this.reply(`${command}\r\n${body}\r\nbadge> `);
    } else if (command === "prov apply") {
      if (!this.badBoot) {
        this.doc.badge_id = JSON.parse(
          this.doc.files["identity.json"].text
        ).badge_id;
        this.doc.provisioned = true;
      }
      this.reply(
        `${command}\r\nPROV OK id=${this.doc.badge_id} name=Example tutorial=reset\r\nbadge> `
      );
    } else if (command === "prov erase confirm") {
      if (this.eraseDenied || this.eraseUnsupported) {
        this.reply(
          `${command}\r\n${
            this.eraseUnsupported ? "Unrecognized command" : "PROV FAIL"
          }\r\nbadge> `
        );
      } else {
        delete this.doc.files["identity.json"];
        this.doc.provisioned = false;
        this.doc.badge_id = "";
        this.hasSyncCredential = false;
        this.reply(`${command}\r\nPROV ERASED\r\nbadge> `);
      }
    } else if (command === "factory_reset confirm") {
      for (const path of Object.keys(this.doc.files)) {
        if (/^(config|apps|appdata)\//.test(path)) delete this.doc.files[path];
      }
      this.reply(
        `${command}\r\nfactory_reset: ${
          this.partialReset ? "partial (some entries failed)" : "cleared"
        }. rebooting...\r\n`
      );
    } else if (command === "snapshot")
      this.reply(
        `snapshot\r\n${JSON.stringify(this.doc)}\r\nbytes=1000\r\nbadge> `
      );
    else if (command === "cat /littlefs/config/system.cfg") {
      this.reply(
        `${command}\r\n${this.doc.files["config/system.cfg"].text.replace(
          /\r?\n/g,
          "\r\n"
        )}\r\nbadge> `
      );
    } else if (command.startsWith("put ")) {
      assert.match(
        command,
        /^put \/littlefs\/(?:config\/system\.cfg|identity\.json) \d+$/
      );
      this.pending = {
        path: command.split(" ")[1].replace("/littlefs/", ""),
        size: Number(command.split(" ").at(-1)),
        received: [],
      };
      this.reply("READY\r\n");
    } else throw new Error(`Unexpected command: ${command}`);
  }
}
async function connection(options, onLost) {
  const port = new FakePort(options);
  const client = new BadgeSerial(onLost);
  await client.connect({ requestPort: async () => port });
  return { port, client };
}

test("USB sync restarts, parses fragmented UTF-8, and never reads badge credentials", async () => {
  const { port, client } = await connection();
  port.doc.files["test.lua"] = {
    text: "Hello 🦆. Prompt: badge> inside the JSON.",
  };
  try {
    assert.deepEqual(await client.snapshot(), port.doc);
    assert.deepEqual(port.commands, ["", "reboot", "snapshot"]);
  } finally {
    await client.disconnect();
  }
  assert.equal(client.connected, false);
  assert.equal(port.closed, true);
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
});

test("the sync page can read badge data without rebooting", async () => {
  const { port, client } = await connection();
  try {
    assert.deepEqual(await client.snapshot({ restart: false }), port.doc);
    assert.deepEqual(port.commands, ["", "snapshot"]);
  } finally {
    await client.disconnect();
  }
});

test("unlock reuses the single synced backup and verifies settings without another snapshot", async () => {
  const { port, client } = await connection();
  try {
    const backup = await client.snapshot();
    const original = structuredClone(backup);
    await client.unlock(backup, () => {});
    assert.deepEqual(backup, original, "the original backup is unchanged");
    assert.notEqual(
      backup.files["config/system.cfg"].text,
      port.doc.files["config/system.cfg"].text
    );
    assert.ok(
      PUBLIC_APPS.every((slug) =>
        fields(port.doc.files["config/system.cfg"].text)
          .launcher_order.split(",")
          .includes(slug)
      )
    );
    assert.ok(port.chunks.length > 1);
    assert.deepEqual(
      port.commands.map((c) => c.split(" ")[0]),
      ["", "reboot", "snapshot", "cat", "put", "cat", "reboot", "cat"]
    );
  } finally {
    await client.disconnect();
  }
});

test("a missing write receipt fails instead of reporting successful unlock", async () => {
  const { port, client } = await connection({ badReceipt: true });
  try {
    await assert.rejects(
      client.unlock(structuredClone(port.doc), () => {}),
      /did not confirm/
    );
    assert.equal(port.commands.at(-1).split(" ")[0], "put");
  } finally {
    await client.disconnect();
  }
});

test("unlock requires a synced backup and rejects changed settings before writing", async () => {
  const { port, client } = await connection();
  const backup = structuredClone(port.doc);
  try {
    await assert.rejects(
      client.unlock(null, () => {}),
      /Sync the connected badge/
    );
    assert.deepEqual(port.commands, [""]);
    port.doc.files["config/system.cfg"].text += "volume=1\n";
    await assert.rejects(
      client.unlock(backup, () => {}),
      /settings changed/
    );
    assert.deepEqual(port.commands, ["", "cat /littlefs/config/system.cfg"]);
  } finally {
    await client.disconnect();
  }
});

test("unlock detects corrupt writes and changes after reboot without taking backups", async () => {
  for (const [options, message, rebootCount] of [
    [{ badReadback: true }, /verification failed/, 0],
    [{ badUnlockBoot: true }, /did not unlock/, 1],
  ]) {
    const { port, client } = await connection(options);
    try {
      await assert.rejects(
        client.unlock(structuredClone(port.doc), () => {}),
        message
      );
      assert.equal(port.commands.filter((c) => c === "snapshot").length, 0);
      assert.equal(
        port.commands.filter((c) => c === "reboot").length,
        rebootCount
      );
    } finally {
      await client.disconnect();
    }
  }
});

test("already unlocked apps need no second write or snapshot", async () => {
  const { port, client } = await connection();
  port.doc.files["config/system.cfg"].text = `launcher_order=${PUBLIC_APPS.join(
    ","
  )}\n`;
  try {
    await client.unlock(structuredClone(port.doc), () => {});
    assert.deepEqual(port.commands, ["", "cat /littlefs/config/system.cfg"]);
  } finally {
    await client.disconnect();
  }
});

test("unplug rejects an outstanding command and releases both stream locks", async () => {
  let lost;
  const { port, client } = await connection({}, (error) => {
    lost = error;
  });
  port.silent = true;
  const waiting = client.snapshot({ restart: false });
  port.dispatchEvent(new Event("disconnect"));
  await assert.rejects(waiting, /unplugged/);
  await client.disconnect();
  assert.match(lost.message, /unplugged/);
  assert.equal(client.connected, false);
  assert.equal(port.readable.locked, false);
  assert.equal(port.writable.locked, false);
});

test("cancelled selection and failed open release resources and can be retried", async () => {
  const client = new BadgeSerial();
  await assert.rejects(
    client.connect({
      requestPort: async () => {
        throw new DOMException("cancelled", "NotFoundError");
      },
    }),
    { name: "NotFoundError" }
  );
  const bad = new FakePort();
  bad.open = async () => {
    throw new Error("port in use");
  };
  await assert.rejects(
    client.connect({ requestPort: async () => bad }),
    /port in use/
  );
  assert.equal(bad.closed, true);
  const port = new FakePort();
  await client.connect({ requestPort: async () => port });
  assert.equal(client.connected, true);
  await client.disconnect();
});

test("timeout rejects instead of returning stale data", async () => {
  const { port, client } = await connection();
  port.silent = true;
  try {
    await assert.rejects(client.ask("snapshot", 5), /did not respond/);
  } finally {
    await client.disconnect();
  }
});

test("production gates reject both mutations before any USB access", async () => {
  for (const env of [
    { DEV: false },
    {
      DEV: false,
      VITE_POST_EVENT_UNLOCK: "false",
      VITE_POST_EVENT_IDENTITY: "false",
    },
  ]) {
    const { BadgeSerial: LockedSerial } = await serialBuild(env);
    const client = new LockedSerial();
    client.sendLine = async () => assert.fail("Must not send a command");
    await assert.rejects(
      client.unlock(null, () => {}),
      /not released/
    );
    await assert.rejects(
      client.saveIdentity(
        null,
        {},
        () => {},
        () => {}
      ),
      /not released/
    );
  }
});

test("identity writing backs up, applies without sync or reboot, and preserves other files", async () => {
  const { port, client } = await connection();
  const files = structuredClone(port.doc.files);
  const before = await client.loadIdentity();
  const draft = {
    ...identityDraft(before.identity),
    display_name: "Grace Example",
    role: "mentor",
    badge_id: "acorn-river-moon-fox",
  };
  let backup;
  try {
    const after = await client.saveIdentity(
      before,
      draft,
      (record) => {
        backup = record;
        assert.ok(!port.commands.some((c) => c.startsWith("put ")));
      },
      () => {}
    );
    assert.deepEqual(backup, before);
    assert.equal(after.badgeId, "acorn-river-moon-fox");
    assert.equal(after.identity.role, "mentor");
    for (const [path, file] of Object.entries(files)) {
      if (path !== "identity.json")
        assert.deepEqual(port.doc.files[path], file);
    }
    assert.ok(
      !port.commands.some((c) => /^(snapshot|reboot|badge_token)/.test(c))
    );
    assert.ok(port.commands.includes("prov apply"));
  } finally {
    await client.disconnect();
  }
});

test("a changed badge or invalid draft is rejected before writing", async () => {
  const { port, client } = await connection();
  const expected = await client.loadIdentity();
  const draft = identityDraft(expected.identity);
  const commands = [...port.commands];
  try {
    await assert.rejects(
      client.saveIdentity(
        expected,
        { ...draft, display_name: "" },
        () => {},
        () => {}
      ),
      /required/
    );
    assert.deepEqual(port.commands, commands);
    port.doc.files["identity.json"].text = JSON.stringify({
      badge_id: "different-badge",
      display_name: "Someone else",
    });
    await assert.rejects(
      client.saveIdentity(
        expected,
        draft,
        () => {},
        () => {}
      ),
      /identity changed/
    );
    assert.ok(!port.commands.some((c) => c.startsWith("put ")));
  } finally {
    await client.disconnect();
  }
});

test("identity readback failure never applies the bad file", async () => {
  const { port, client } = await connection({ badReadback: true });
  const expected = await client.loadIdentity();
  try {
    await assert.rejects(
      client.saveIdentity(
        expected,
        identityDraft(expected.identity),
        () => {},
        () => {}
      ),
      /readback failed/
    );
    assert.ok(!port.commands.includes("prov apply"));
    assert.ok(!port.commands.includes("reboot"));
  } finally {
    await client.disconnect();
  }
});

test("identity verification rejects a file the badge did not apply", async () => {
  const { port, client } = await connection({ badBoot: true });
  const expected = await client.loadIdentity();
  try {
    await assert.rejects(
      client.saveIdentity(
        expected,
        {
          ...identityDraft(expected.identity),
          badge_id: "acorn-river-moon-fox",
        },
        () => {},
        () => {}
      ),
      /did not load/
    );
    assert.ok(!port.commands.includes("reboot"));
  } finally {
    await client.disconnect();
  }
});

test("an unregistered badge receives identity without snapshot, online sync, unlock or reboot", async () => {
  const { port, client } = await connection();
  delete port.doc.files["identity.json"];
  port.doc.provisioned = false;
  port.doc.badge_id = "";
  try {
    const expected = await client.loadIdentity();
    assert.equal(expected.raw, null);
    const after = await client.saveIdentity(
      expected,
      {
        ...identityDraft(),
        display_name: "Fresh badge",
        badge_id: "acorn-river-moon-fox",
        role: "visitor",
      },
      () => {},
      () => {}
    );
    assert.equal(after.provisioned, true);
    assert.equal(after.badgeId, "acorn-river-moon-fox");
    assert.ok(
      !port.commands.some((c) =>
        /^(snapshot|reboot|seedall|badge_token)/.test(c)
      )
    );
  } finally {
    await client.disconnect();
  }
});

test("assigned badges with unreadable identity are not treated as blank", async () => {
  const { port, client } = await connection();
  try {
    delete port.doc.files["identity.json"];
    await assert.rejects(client.loadIdentity(), /could not be read/);
    port.doc.files["identity.json"] = { text: "bad json" };
    await assert.rejects(client.loadIdentity(), /not valid JSON/);
    assert.ok(!port.commands.some((c) => c.startsWith("put ")));
  } finally {
    await client.disconnect();
  }
});

test("missing identity supports firmware with and without error-name lookup", async () => {
  for (const missingFileDiagnostic of [false, "UNKNOWN ERROR"]) {
    const { port, client } = await connection({ missingFileDiagnostic });
    delete port.doc.files["identity.json"];
    port.doc.provisioned = false;
    port.doc.badge_id = "";
    try {
      assert.deepEqual(await client.loadIdentity(), {
        raw: null,
        identity: {},
        provisioned: false,
        badgeId: "",
      });
    } finally {
      await client.disconnect();
    }
  }
});

test("unregistered badges with actual read errors cannot silently overwrite identity", async () => {
  for (const identityReadError of ["Input/output error", "Permission denied"]) {
    const { port, client } = await connection({ identityReadError });
    port.doc.provisioned = false;
    try {
      await assert.rejects(client.loadIdentity(), /could not be read/);
      assert.ok(!port.commands.some((command) => command.startsWith("put ")));
    } finally {
      await client.disconnect();
    }
  }
});

test("a factory-reset badge can reconnect and bootstrap identity with no sync or unlock", async () => {
  const { port, client } = await connection();
  const wallet = structuredClone(port.doc.files["solana.json"]);
  try {
    await client.factoryReset(true);
    assert.equal(client.connected, false);
    await client.connect({ requestPort: async () => port });
    const expected = await client.loadIdentity();
    assert.deepEqual(expected, {
      raw: null,
      identity: {},
      provisioned: false,
      badgeId: "",
    });
    let backup;
    const after = await client.saveIdentity(
      expected,
      {
        ...identityDraft(),
        display_name: "New owner",
        badge_id: "acorn-river-moon-fox",
        role: "hacker",
      },
      (record) => {
        backup = record;
      },
      () => {}
    );
    assert.deepEqual(backup, expected);
    assert.equal(after.provisioned, true);
    assert.equal(after.identity.display_name, "New owner");
    assert.equal(after.badgeId, "acorn-river-moon-fox");
    assert.deepEqual(port.doc.files["solana.json"], wallet);
    assert.ok(
      !port.commands.some((command) =>
        /^(snapshot|reboot|seedall|badge_token)/.test(command)
      )
    );
  } finally {
    await client.disconnect();
  }
});

test("dashboard-style full reset removes identity and credential, preserves wallet, and needs no post-reboot prompt", async () => {
  const { port, client } = await connection();
  port.doc.files["solana.json"] = { text: "test wallet fixture" };
  await assert.rejects(client.factoryReset(false), /Confirm/);
  assert.deepEqual(port.commands, [""]);
  await client.factoryReset(true);
  assert.equal(client.connected, false);
  assert.equal(port.closed, true);
  assert.equal(port.doc.files["identity.json"], undefined);
  assert.equal(port.doc.provisioned, false);
  assert.equal(port.hasSyncCredential, false);
  assert.equal(port.doc.files["solana.json"].text, "test wallet fixture");
  assert.ok(
    !Object.keys(port.doc.files).some((path) =>
      /^(config|apps|appdata)\//.test(path)
    )
  );
  assert.deepEqual(port.commands, [
    "",
    "prov erase confirm",
    "factory_reset confirm",
  ]);
});

test("partial factory reset explicitly reports that identity was already erased", async () => {
  const { port, client } = await connection({ partialReset: true });
  try {
    await assert.rejects(
      client.factoryReset(true),
      /identity and sync credential were erased, but the rest of the reset may be incomplete/
    );
    assert.equal(port.doc.files["identity.json"], undefined);
    assert.equal(port.hasSyncCredential, false);
  } finally {
    await client.disconnect();
  }
});

test("full reset stops before clearing other data if identity erase is denied or unsupported", async () => {
  for (const options of [{ eraseDenied: true }, { eraseUnsupported: true }]) {
    const { port, client } = await connection(options);
    const before = structuredClone(port.doc);
    try {
      await assert.rejects(
        client.factoryReset(true),
        /did not confirm identity removal/
      );
      assert.deepEqual(port.commands, ["", "prov erase confirm"]);
      assert.deepEqual(port.doc, before);
      assert.equal(port.hasSyncCredential, true);
    } finally {
      await client.disconnect();
    }
  }
});
