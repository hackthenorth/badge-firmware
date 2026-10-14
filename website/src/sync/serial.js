import { parseSnapshot, planUnlock, PUBLIC_APPS, fields } from "./data.js";
import { FEATURES } from "./features.js";
import { buildIdentity } from "./identity.js";

const PROMPT = "badge> ";
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const clean = (text) => text.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g, "");

export function parseSnapshotReply(reply) {
  const lines = clean(reply).replace(/\r/g, "").split("\n");
  const line = lines.find((line) => line.trimStart().startsWith("{"));
  if (!line) {
    if (reply.includes("no room") || reply.includes("out of memory"))
      throw new Error(
        "The badge ran out of memory. Unplug it, reconnect, and sync before opening an app."
      );
    throw new Error(
      "No snapshot received. Keep the badge switched OFF and connected by USB, then retry."
    );
  }
  return parseSnapshot(line.trim());
}

// Single owner / single operation. Never logs snapshot contents or reads tokens.
export class BadgeSerial {
  constructor(onLost = () => {}) {
    this.onLost = onLost;
    this.rx = "";
    this.failure = null;
  }
  get connected() {
    return Boolean(this.port && this.writer && !this.failure);
  }

  async connect(serial = navigator.serial) {
    if (!serial)
      throw new Error("Use desktop Chrome or Edge to connect over USB.");
    try {
      this.port = await serial.requestPort();
      await this.port.open({ baudRate: 115200, bufferSize: 65536 });
      this.failure = null;
      this.writer = this.port.writable.getWriter();
      this.reader = this.port.readable.getReader();
      this.disconnectHandler = () =>
        this.lost(
          new Error(
            "Badge unplugged. Your downloaded data is still available below."
          )
        );
      this.port.addEventListener("disconnect", this.disconnectHandler);
      this.reading = this.readLoop();
      for (let attempt = 0; attempt < 3; attempt++) {
        this.rx = "";
        await this.writer.write(new Uint8Array([3]));
        await this.sendLine("");
        try {
          await this.waitFor(PROMPT, 1500);
          return;
        } catch (error) {
          if (this.failure || attempt === 2) throw error;
        }
      }
    } catch (error) {
      await this.disconnect();
      throw error;
    }
  }

  async readLoop() {
    const decoder = new TextDecoder();
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        this.rx += decoder.decode(value, { stream: true });
        if (this.rx.length > 262144)
          throw new Error(
            "The badge response exceeded the serial buffer. Reconnect and retry."
          );
      }
      if (!this.closing)
        this.lost(new Error("USB connection ended. Reconnect your badge."));
    } catch (error) {
      if (!this.closing) this.lost(error);
    }
  }

  lost(error) {
    if (this.failure || this.closing) return;
    this.failure = error;
    this.onLost(error);
    // Do not await cleanup from readLoop: cleanup itself awaits readLoop.
    void this.disconnect();
  }

  async disconnect() {
    if (this.closing) return this.closing;
    this.closing = (async () => {
      this.port?.removeEventListener("disconnect", this.disconnectHandler);
      try {
        await this.reader?.cancel();
      } catch {
        /* already disconnected */
      }
      try {
        await this.reading;
      } catch {
        /* read error already reported */
      }
      try {
        this.reader?.releaseLock();
      } catch {
        /* no reader */
      }
      try {
        this.writer?.releaseLock();
      } catch {
        /* no writer */
      }
      try {
        await this.port?.close();
      } catch {
        /* unplugged */
      }
      this.reader = this.writer = this.port = this.reading = null;
      this.rx = "";
    })();
    await this.closing;
    this.closing = null;
  }

  async waitFor(token, timeout = 5000) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      if (this.failure) throw this.failure;
      if (!this.port || this.closing)
        throw new Error("The badge is disconnected. Reconnect to continue.");
      const index = this.rx.indexOf(token);
      if (index >= 0) {
        const result = this.rx.slice(0, index + token.length);
        this.rx = this.rx.slice(index + token.length);
        return result;
      }
      await pause(20);
    }
    throw new Error(
      "The badge did not respond in time. Unplug it, reconnect, and try again."
    );
  }

  async sendLine(line) {
    if (!this.connected) throw new Error("Connect your badge first.");
    const bytes = new TextEncoder().encode(line + "\r"); // bare CR, never CRLF
    if (bytes.length >= 256) throw new Error("Console command is too long.");
    await this.writer.write(bytes);
  }

  async ask(line, timeout = 5000) {
    this.rx = "";
    await this.sendLine(line);
    // A prompt embedded in JSON (e.g. Lua source) is not the REPL terminator.
    const deadline = Date.now() + timeout;
    let reply = "";
    while (Date.now() < deadline) {
      reply += await this.waitFor(PROMPT, deadline - Date.now());
      if (/(?:^|[\r\n])(?:\x1b\[[0-?]*[ -/]*[@-~])*badge> $/.test(reply))
        return reply;
    }
    throw new Error("The badge response was incomplete. Reconnect and retry.");
  }

  async reboot() {
    this.rx = "";
    await this.sendLine("reboot");
    await this.waitFor("rebooting...", 3000);
    await this.waitFor(PROMPT, 20000);
  }

  async snapshot({ restart = true } = {}) {
    if (restart) await this.reboot();
    return parseSnapshotReply(await this.ask("snapshot", 30000));
  }

  async unlock(before, onStatus) {
    if (!FEATURES.unlock) throw new Error("App unlocking is not released yet.");
    if (!before)
      throw new Error("Sync the connected badge before unlocking apps.");
    const plan = planUnlock(before);
    // The initial sync is the only backup. Read just the launcher settings to
    // guard against stale data and verify the write without another snapshot.
    onStatus("Checking your badge’s app settings…");
    if (
      (await this.#readLauncherConfig()) !==
      plan.original.replace(/\r\n/g, "\n")
    )
      throw new Error(
        "Your badge settings changed. Sync again before unlocking apps."
      );
    if (!plan.added.length) return;
    onStatus("Unlocking apps… Keep the USB cable connected.");
    await this.#writeFile("/littlefs/config/system.cfg", plan.updated);
    if ((await this.#readLauncherConfig()) !== plan.updated)
      throw new Error(
        "Launcher verification failed. Keep your before-unlock backup and reconnect."
      );
    onStatus("Restarting and checking your unlocked apps…");
    await this.reboot();
    const after = await this.#readLauncherConfig();
    const order = (fields(after).launcher_order || "").split(",");
    if (
      after !== plan.updated ||
      !PUBLIC_APPS.every((slug) => order.includes(slug))
    )
      throw new Error(
        "Some apps did not unlock. Keep the backup and try again."
      );
  }

  async #readLauncherConfig() {
    const command = "cat /littlefs/config/system.cfg";
    const reply = clean(await this.ask(command)).replace(/\r\n/g, "\n");
    // The console echoes the command and appends one newline after the file.
    if (!reply.startsWith(`${command}\n`) || !reply.endsWith(`\n${PROMPT}`))
      throw new Error(
        "Couldn’t verify your badge settings. Reconnect and sync again."
      );
    return reply.slice(command.length + 1, -(PROMPT.length + 1));
  }

  // Reading identity is deliberately independent of snapshot/server sync. A
  // badge with no identity can be registered through this path over USB.
  async loadIdentity() {
    const info = fields(clean(await this.ask("prov show")));
    if (info.provisioned !== "0" && info.provisioned !== "1")
      throw new Error(
        "The badge did not report its identity. Reconnect and try again."
      );
    const raw = await this.#readFile("/littlefs/identity.json", {
      allowMissing: info.provisioned === "0",
    });
    let identity = {};
    if (raw !== null) {
      if (new TextEncoder().encode(raw).length > 4096)
        throw new Error("The identity file is too large to edit safely.");
      try {
        identity = JSON.parse(raw);
      } catch {
        throw new Error(
          "The badge identity file is not valid JSON. Keep a backup before repairing it."
        );
      }
      if (!identity || typeof identity !== "object" || Array.isArray(identity))
        throw new Error("The badge identity file is invalid.");
    }
    return {
      raw,
      identity,
      provisioned: info.provisioned === "1",
      badgeId: info.badge_id || "",
    };
  }

  async saveIdentity(expected, draft, onBackup, onStatus) {
    if (!FEATURES.identity)
      throw new Error("Identity writing is not released yet.");
    if (!expected)
      throw new Error("Load the connected badge’s identity before saving.");
    const identity = buildIdentity(expected.identity, draft);
    onStatus("Checking the badge identity…");
    const before = await this.loadIdentity();
    if (
      before.raw !== expected.raw ||
      before.badgeId !== expected.badgeId ||
      before.provisioned !== expected.provisioned
    )
      throw new Error(
        "The badge identity changed. Reload its identity before saving your edits."
      );
    await onBackup(before);
    const json = JSON.stringify(identity);
    onStatus("Uploading your identity… Keep the USB cable connected.");
    await this.#writeFile("/littlefs/identity.json", json);
    if ((await this.#readFile("/littlefs/identity.json")) !== json)
      throw new Error(
        "Identity readback failed. Your previous identity is saved locally; reconnect before retrying."
      );
    onStatus("Applying your identity without restarting…");
    const applied = clean(await this.ask("prov apply", 15000));
    if (!/(?:^|[\r\n])PROV OK id=/.test(applied))
      throw new Error(
        "The badge could not apply the identity. Reconnect and reload its identity before retrying."
      );
    const after = await this.loadIdentity();
    if (
      !after.provisioned ||
      after.badgeId !== identity.badge_id ||
      after.raw !== json
    )
      throw new Error(
        "The badge did not load the new identity. Reconnect and reload its identity before retrying."
      );
    return after;
  }

  async factoryReset(confirmed, onStatus = () => {}) {
    if (confirmed !== true)
      throw new Error("Confirm the data deletion before resetting your badge.");
    // Match the dashboard: erase registration and its NVS sync credential first.
    // The firmware's factory_reset alone deliberately preserves identity.json.
    onStatus("Removing your badge identity and sync credential…");
    const erased = clean(await this.ask("prov erase confirm", 8000));
    if (!/(?:^|[\r\n])PROV ERASED(?:[\r\n])/.test(erased))
      throw new Error(
        "The badge did not confirm identity removal. Reset stopped. Reconnect and inspect it before trying again."
      );
    try {
      onStatus(
        "Clearing your badge data… Keep USB connected while it restarts."
      );
      this.rx = "";
      await this.sendLine("factory_reset confirm");
      // Reset reboots immediately, so its receipt may be the last USB output.
      // Do not wait for a new console prompt after the badge restarts.
      const reply = clean(await this.waitFor("rebooting...", 30000));
      if (!/(?:^|[\r\n])factory_reset: cleared\. rebooting\.\.\./.test(reply))
        throw new Error("Reset was not confirmed.");
    } catch {
      throw new Error(
        "Your badge identity and sync credential were erased, but the rest of the reset may be incomplete. Reconnect and retry the reset before setting up your identity."
      );
    }
    await this.disconnect();
  }

  async #readFile(path, { allowMissing = false } = {}) {
    const command = `cat ${path}`;
    const reply = clean(await this.ask(command)).replace(/\r\n/g, "\n");
    if (!reply.startsWith(`${command}\n`) || !reply.endsWith(`\n${PROMPT}`))
      throw new Error(
        "The badge file response was incomplete. Reconnect and try again."
      );
    const body = reply.slice(command.length + 1, -(PROMPT.length + 1));
    // ESP-IDF appends this diagnostic when cat returns 1 for a missing file.
    // Recognize ENOENT only when prov show says the badge is unregistered;
    // other read failures must never be mistaken for a blank identity.
    const missingReply = body
      .trimEnd()
      .replace(
        /\nCommand returned non-zero error code: 0x1 \((?:UNKNOWN )?ERROR\)$/,
        ""
      );
    if (
      allowMissing &&
      missingReply === `open ${path}: No such file or directory`
    )
      return null;
    if (body.startsWith(`open ${path}:`))
      throw new Error(
        "The badge file could not be read. Reconnect and try again."
      );
    return body;
  }

  async #writeFile(path, text) {
    const bytes = new TextEncoder().encode(text);
    this.rx = "";
    await this.sendLine(`put ${path} ${bytes.length}`);
    await this.waitFor("READY", 5000);
    // The native USB RX ring is 256 bytes. Pace smaller chunks like the IDE.
    for (let i = 0; i < bytes.length; i += 128) {
      await this.writer.write(bytes.slice(i, i + 128));
      await pause(20);
    }
    const receipt = clean(await this.waitFor(PROMPT, 10000));
    if (
      !new RegExp(`(?:^|[\\r\\n])OK ${bytes.length}(?:[\\r\\n])`).test(receipt)
    ) {
      throw new Error(
        "The badge did not confirm the write. Keep your backup and reconnect."
      );
    }
  }
}
