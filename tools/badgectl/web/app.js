"use strict";

const STORAGE_KEY = "badgectl:files:v1";
const LAYOUT_KEY = "badgectl:layout:v1";
const PERSIST_INTERVAL_MS = 2000;
const NO_PUSH = new Set(["README.md"]);

const state = {
  port: null,
  writer: null,
  reader: null,
  readBuffer: "",
  files: new Map(),   // path -> string content
  models: new Map(),  // path -> monaco.editor.ITextModel
  currentPath: null,
  editor: null,
  history: [],        // console input history (newest at end)
  historyIdx: 0,      // index into history; equal to length means "draft"
};

const isPushable = (path) => !NO_PUSH.has(path);
// Binary files (currently icon.bin) are stored as Uint8Array in state.files.
const isBinaryValue = (v) => v instanceof Uint8Array;
const ICON_BIN = "icon.bin";
const ICON_SIZE = 42; // Final launcher size: no resizing needed on the badge.
// LVGL v9 file-image header, cf RGB565A8 (0x14), uncompressed. Mirrors
// tools/image-convert/LVGLImage.py::to_bin with CompressMethod.NONE.
const LVGL_MAGIC = 0x19;
const LVGL_CF_RGB565A8 = 0x14;

function bytesToB64(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

function b64ToBytes(b64) {
  const s = atob(b64);
  const out = new Uint8Array(s.length);
  for (let i = 0; i < s.length; i++) out[i] = s.charCodeAt(i);
  return out;
}

// Pack thumbnail RGBA pixels (canvas ImageData layout) into an LVGL
// RGB565A8 .bin: 12-byte header, RGB565LE pixels, then the alpha map.
// Same quantization as LVGLImage.py (_png_to_colormap, no dither).
function encodeIconBin(rgba, w, h) {
  const px = w * h;
  const out = new Uint8Array(12 + px * 2 + px);
  out[0] = LVGL_MAGIC;
  out[1] = LVGL_CF_RGB565A8;
  out[2] = 0; // flags: uncompressed, not premultiplied
  out[3] = 0;
  out[4] = w & 0xff;
  out[5] = (w >> 8) & 0xff;
  out[6] = h & 0xff;
  out[7] = (h >> 8) & 0xff;
  const stride = w * 2;
  out[8] = stride & 0xff;
  out[9] = (stride >> 8) & 0xff;
  out[10] = 0;
  out[11] = 0;
  for (let i = 0; i < px; i++) {
    const r = rgba[i * 4];
    const g = rgba[i * 4 + 1];
    const b = rgba[i * 4 + 2];
    const c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    out[12 + i * 2] = c & 0xff;
    out[12 + i * 2 + 1] = (c >> 8) & 0xff;
    out[12 + px * 2 + i] = rgba[i * 4 + 3];
  }
  return out;
}

// Inverse of encodeIconBin for preview. Returns {rgba, w, h} or null.
function decodeIconBin(bin) {
  if (!(bin instanceof Uint8Array) || bin.length < 12) return null;
  if (bin[0] !== LVGL_MAGIC || bin[1] !== LVGL_CF_RGB565A8) return null;
  const w = bin[4] | (bin[5] << 8);
  const h = bin[6] | (bin[7] << 8);
  const stride = bin[8] | (bin[9] << 8);
  // Accept saved 48px thumbnails so loadPersisted can resize older workspaces.
  if ((w !== ICON_SIZE && w !== 48) || h !== w || stride !== w * 2) return null;
  if (bin.length !== 12 + w * h * 2 + w * h) return null;
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const c = bin[12 + i * 2] | (bin[12 + i * 2 + 1] << 8);
    rgba[i * 4] = (((c >> 11) & 0x1f) * 255 + 15) / 31 | 0;
    rgba[i * 4 + 1] = (((c >> 5) & 0x3f) * 255 + 31) / 63 | 0;
    rgba[i * 4 + 2] = ((c & 0x1f) * 255 + 15) / 31 | 0;
    rgba[i * 4 + 3] = bin[12 + w * h * 2 + i];
  }
  return { rgba, w, h };
}

function imageToIconBin(image) {
  const work = document.createElement("canvas");
  work.width = work.height = ICON_SIZE;
  const ctx = work.getContext("2d");
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = "high";
  // Crop to a square and resize in the browser, keeping transparency.
  const scale = Math.max(ICON_SIZE / image.width, ICON_SIZE / image.height);
  const dw = image.width * scale;
  const dh = image.height * scale;
  ctx.drawImage(image, (ICON_SIZE - dw) / 2, (ICON_SIZE - dh) / 2, dw, dh);
  return encodeIconBin(ctx.getImageData(0, 0, ICON_SIZE, ICON_SIZE).data, ICON_SIZE, ICON_SIZE);
}

const $ = (id) => document.getElementById(id);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Host->badge pacing. The badge's USB-Serial-JTAG RX ring is 256 bytes and
// cannot be grown: a burst over ~256 B is silently truncated, and cmd_put
// then blocks forever waiting for dropped bytes (firmware has no read
// timeout). So every payload goes out in small chunks with pauses — the same
// 128 B / 20 ms shape as tools/badge/badge.py and tools/badge-provision.
const WRITE_CHUNK = 128;
const WRITE_PAUSE_MS = 20;

function setStatus(text, kind) {
  const el = $("status");
  el.textContent = text;
  el.className = kind || "";
}

function appendLog(text, kind) {
  const log = $("log");
  const span = document.createElement("span");
  if (kind) span.className = kind;
  span.textContent = text;
  log.appendChild(span);
  log.scrollTop = log.scrollHeight;
}

function languageFor(path) {
  if (path.endsWith(".lua")) return "lua";
  if (path.endsWith(".json")) return "json";
  if (path.endsWith(".cfg") || path.endsWith(".ini")) return "ini";
  if (path.endsWith(".md")) return "markdown";
  return "plaintext";
}

function persist() {
  const obj = {};
  for (const [p, c] of state.files) {
    obj[p] = isBinaryValue(c) ? { __bin: true, data: bytesToB64(c) } : c;
  }
  for (const [p, m] of state.models) {
    if (isBinaryValue(state.files.get(p))) continue;
    obj[p] = m.getValue();
  }
  localStorage.setItem(STORAGE_KEY, JSON.stringify(obj));
}

function loadPersisted() {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return false;
    const obj = JSON.parse(raw);
    for (const [p, c] of Object.entries(obj)) {
      if (c && typeof c === "object" && c.__bin === true && typeof c.data === "string") {
        let bin = b64ToBytes(c.data);
        const oldIcon = p === ICON_BIN ? decodeIconBin(bin) : null;
        if (oldIcon && oldIcon.w !== ICON_SIZE) {
          const canvas = document.createElement("canvas");
          canvas.width = oldIcon.w;
          canvas.height = oldIcon.h;
          canvas.getContext("2d").putImageData(new ImageData(oldIcon.rgba, oldIcon.w, oldIcon.h), 0, 0);
          bin = imageToIconBin(canvas);
        }
        state.files.set(p, bin);
      } else {
        state.files.set(p, c);
      }
    }
    return state.files.size > 0;
  } catch {
    return false;
  }
}

function defaultReadme() {
  const el = document.getElementById("default-readme");
  return el ? el.textContent.replace(/^\n+/, "") : "";
}

function seedDefaults() {
  state.files.set("manifest.cfg", "slug=hello\nname=Hello\nicon=HI\napi=2\n");
  state.files.set(
    "main.lua",
    [
      '-- Lua entrypoint for the "hello" app.',
      "function on_enter(root)",
      '  local lbl = badge.ui.label(root, "hello, world")',
      '  lbl:set_font_size("large")',
      '  lbl:align("center", 0, 0)',
      "end",
      "",
      "function on_button(b, kind)",
      "  if kind ~= badge.input.KIND.PRESSED then return end",
      "  -- handle buttons here",
      "end",
      "",
    ].join("\n"),
  );
  state.files.set("README.md", defaultReadme());
}

async function refreshGuide() {
  try {
    const response = await fetch("README.md", { cache: "no-cache" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const guide = await response.text();
    state.files.set("README.md", guide);
    const model = state.models.get("README.md");
    if (model) model.setValue(guide);
  } catch {
    // Keep a cached guide when offline, or the embedded quick-start fallback.
    if (!state.files.has("README.md")) state.files.set("README.md", defaultReadme());
  }
  rebuildFileList();
}

function previewAppImport() {
  try {
    const app = BadgeAppBundle.parse($("app-source").value);
    $("import-preview").textContent = `Ready: ${app.slug} — manifest.cfg + main.lua`;
    $("import-preview").className = "ok";
    $("apply-app-import").disabled = false;
  } catch (e) {
    $("import-preview").textContent = e.message;
    $("import-preview").className = "err";
    $("apply-app-import").disabled = true;
  }
}

function importApp() {
  try {
    // Validate before changing the workspace; importing never sends USB data.
    const app = BadgeAppBundle.parse($("app-source").value);
    const guide = state.files.get("README.md") || defaultReadme();
    state.editor.setModel(null);
    for (const model of state.models.values()) model.dispose();
    state.models.clear();
    state.files.clear();
    state.files.set("manifest.cfg", app.manifest);
    state.files.set("main.lua", app.main);
    state.files.set("README.md", guide);
    refreshSlug();
    openFile("main.lua");
    renderIconPreview();
    persist();
    $("import-app-dialog").close();
    setStatus(`imported ${app.slug} — Connect and Push to install`, "ok");
  } catch (e) {
    $("import-preview").textContent = e.message;
    $("import-preview").className = "err";
  }
}

function downloadApp() {
  try {
    const extras = [...state.files.keys()].filter(p => isPushable(p) && p !== "manifest.cfg" && p !== "main.lua");
    if (extras.length) throw new Error("Single-file download needs only manifest.cfg + main.lua. Save extra files separately first: " + extras.join(", "));
    const content = (p) => state.models.get(p)?.getValue() ?? state.files.get(p) ?? "";
    const source = BadgeAppBundle.serialize(content("manifest.cfg"), content("main.lua"));
    const slug = BadgeAppBundle.parse(source).slug;
    const url = URL.createObjectURL(new Blob([source], { type: "text/plain;charset=utf-8" }));
    const link = document.createElement("a");
    link.href = url;
    link.download = `${slug}.lua`;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    setStatus(`downloaded ${slug}.lua`, "ok");
  } catch (e) {
    setStatus(e.message, "err");
  }
}

function refreshSlug() {
  const manifest = state.files.get("manifest.cfg") ?? "";
  const m = manifest.match(/^\s*slug\s*=\s*(\S+)/m);
  $("slug-readout").textContent = m ? `slug=${m[1]}` : "(no slug)";
}

function rebuildFileList() {
  const list = $("files");
  list.innerHTML = "";
  const paths = [...state.files.keys()].sort();
  for (const p of paths) {
    const li = document.createElement("li");
    if (p === state.currentPath) li.classList.add("active");
    if (!isPushable(p)) li.classList.add("no-push");
    li.title = isPushable(p) ? p : `${p} (not pushed to badge)`;
    li.onclick = () => openFile(p);

    const label = document.createElement("span");
    const v = state.files.get(p);
    label.textContent = isBinaryValue(v) ? `${p} (${v.length} B)` : p;
    li.appendChild(label);

    const del = document.createElement("button");
    del.className = "del";
    del.textContent = "×";
    del.title = "delete";
    del.onclick = (e) => {
      e.stopPropagation();
      deleteFile(p);
    };
    li.appendChild(del);

    list.appendChild(li);
  }
}

function openFile(path) {
  if (isBinaryValue(state.files.get(path))) {
    // Binary files have no text view; keep them managed via their own panel.
    if (!state.models.has(path)) {
      const info =
        `${path} is a binary file (${state.files.get(path).length} bytes).\n` +
        "It is pushed to the badge as-is and is not editable here.";
      state.models.set(path, monaco.editor.createModel(info, "plaintext"));
    }
    state.editor.setModel(state.models.get(path));
    state.currentPath = path;
    rebuildFileList();
    return;
  }
  if (!state.models.has(path)) {
    const content = state.files.get(path) ?? "";
    const model = monaco.editor.createModel(content, languageFor(path));
    model.onDidChangeContent(() => {
      state.files.set(path, model.getValue());
      if (path === "manifest.cfg") {
        refreshSlug();
        renderIconPreview();
      }
    });
    state.models.set(path, model);
  }
  state.editor.setModel(state.models.get(path));
  state.currentPath = path;
  rebuildFileList();
}

function addFile() {
  const path = prompt("new file path (relative, e.g. lib/util.lua):");
  if (!path) return;
  const clean = path.trim().replace(/^\/+/, "");
  if (!clean) return;
  if (state.files.has(clean)) {
    openFile(clean);
    return;
  }
  state.files.set(clean, "");
  refreshSlug();
  rebuildFileList();
  openFile(clean);
  persist();
}

function deleteFile(path) {
  if (!confirm(`delete ${path}?`)) return;
  state.files.delete(path);
  const model = state.models.get(path);
  if (model) {
    model.dispose();
    state.models.delete(path);
  }
  if (state.currentPath === path) {
    state.currentPath = null;
    state.editor.setModel(null);
  }
  refreshSlug();
  rebuildFileList();
  renderIconPreview();
  persist();
}

async function connect() {
  if (!("serial" in navigator)) {
    setStatus("Web Serial not available in this browser", "err");
    return;
  }
  try {
    setStatus("choose USB JTAG/serial debug unit…", "busy");
    state.port = await navigator.serial.requestPort();
    await state.port.open({ baudRate: 115200, bufferSize: 4096 });
    state.writer = state.port.writable.getWriter();
    startReadLoop();
    setStatus("connected", "ok");
    $("connect").textContent = "Disconnect";
    $("push").disabled = false;
    $("reboot").disabled = false;
    $("break").disabled = false;
    $("console-input").disabled = false;
  } catch (e) {
    state.port = null;
    if (e && e.name === "NotFoundError") {
      setStatus("connection cancelled");
    } else {
      setStatus("connect failed: " + e.message, "err");
    }
  }
}

function openConnectGuide() {
  const guide = $("connect-guide");
  if (typeof guide.showModal === "function") {
    guide.showModal();
  } else {
    void connect();
  }
}

async function disconnect() {
  try {
    if (state.reader) await state.reader.cancel();
  } catch {}
  try {
    if (state.writer) state.writer.releaseLock();
  } catch {}
  try {
    if (state.port) await state.port.close();
  } catch {}
  state.port = null;
  state.writer = null;
  state.reader = null;
  setStatus("disconnected");
  $("connect").textContent = "Connect";
  $("push").disabled = true;
  $("reboot").disabled = true;
  $("break").disabled = true;
  $("console-input").disabled = true;
}

async function startReadLoop() {
  const decoder = new TextDecoder();
  state.reader = state.port.readable.getReader();
  try {
    while (true) {
      const { value, done } = await state.reader.read();
      if (done) break;
      const text = decoder.decode(value, { stream: true });
      state.readBuffer += text;
      // The matcher buffer only ever needs recent output (every wait starts
      // from a cleared buffer), so cap it: hours of app log spam would
      // otherwise grow indexOf scans — and stale "READY"/"OK n" lines could
      // false-match a later transfer.
      if (state.readBuffer.length > 32768) {
        state.readBuffer = state.readBuffer.slice(-32768);
      }
      appendLog(text);
    }
  } catch (e) {
    appendLog("\n[read error] " + e.message + "\n", "err");
  } finally {
    try {
      state.reader.releaseLock();
    } catch {}
  }
}

async function sendLine(s) {
  // Bare CR: the console expects ESP_LINE_ENDINGS_CR (cf tools/badge/badge.py).
  // The old "\r\n" left a stray empty command after every line.
  await state.writer.write(new TextEncoder().encode(s + "\r"));
  appendLog(">> " + s + "\n", "sent");
}

async function sendBytes(bytes) {
  // Chunked + paced: one big write overflows the badge's 256-byte RX ring
  // and wedges cmd_put. See WRITE_CHUNK above.
  for (let i = 0; i < bytes.length; i += WRITE_CHUNK) {
    await state.writer.write(bytes.slice(i, i + WRITE_CHUNK));
    if (i + WRITE_CHUNK < bytes.length) await sleep(WRITE_PAUSE_MS);
  }
}

async function sendBreak() {
  if (!state.writer) return;
  await state.writer.write(new Uint8Array([0x03])); // Ctrl-C
  appendLog(">> ^C\n", "sent");
}

// Wait until pattern appears in the read buffer (or timeout).
// Consumes the buffer up to and including the match.
async function waitFor(pattern, timeoutMs = 5000) {
  const start = performance.now();
  while (performance.now() - start < timeoutMs) {
    const idx = state.readBuffer.indexOf(pattern);
    if (idx >= 0) {
      const response = state.readBuffer.slice(0, idx + pattern.length);
      state.readBuffer = state.readBuffer.slice(idx + pattern.length);
      return response;
    }
    await sleep(20);
  }
  throw new Error(`timeout waiting for "${pattern}"`);
}

function getSlug() {
  const manifest = state.files.get("manifest.cfg") ?? "";
  const m = manifest.match(/^\s*slug\s*=\s*(\S+)/m);
  if (!m) throw new Error("manifest.cfg is missing a slug= line");
  return m[1];
}

// Try to leave the badge at a live prompt after a failed push.
// If the badge is stuck inside cmd_put's blocking read, feed it the most it
// could still be waiting for (pendingBytes is an upper bound: the badge takes
// what it needs, prints OK, and returns to the REPL; the leftover pad bytes
// land as one garbage command line). The interrupted file is corrupt, but the
// next push overwrites it whole. Safe no-op when the badge was never in put.
async function resyncAfterPush(pendingBytes) {
  try {
    if (pendingBytes > 0) {
      setStatus("resyncing badge…", "busy");
      appendLog(`[resync] sending ${pendingBytes} pad bytes to release the badge…\n`, "info");
      await sendBytes(new Uint8Array(pendingBytes));
    }
    state.readBuffer = "";
    for (let i = 0; i < 3; i++) {
      await sendLine("");
      try {
        await waitFor("badge> ", 1500);
        appendLog("[resync] badge is responding\n", "ok");
        return true;
      } catch {}
    }
    return false;
  } catch {
    return false;
  }
}

async function push() {
  if (!state.writer) {
    setStatus("not connected", "err");
    return;
  }
  // Sync any unsaved editor edits into the file map (text files only;
  // binary entries such as icon.bin are managed via their own panel).
  for (const [p, m] of state.models) {
    if (isBinaryValue(state.files.get(p))) continue;
    state.files.set(p, m.getValue());
  }

  $("push").disabled = true;
  $("import-app").disabled = true;
  setStatus("pushing…", "busy");
  // Size of the file currently being received by the badge (0 outside a put).
  // Used to unwedge cmd_put if the transfer fails mid-file; see resync.
  let pendingBytes = 0;
  try {
    const slug = getSlug();
    const remoteDir = `/littlefs/apps/${slug}`;

    // Fail fast: if the badge isn't at a prompt (wedged from an earlier
    // transfer?), don't feed it commands it will misinterpret.
    state.readBuffer = "";
    await sendLine("");
    await waitFor("badge> ", 3000);

    // Older firmware translates CR bytes even inside put's binary payload.
    // Check before changing any files rather than reporting a corrupt icon
    // as a successful upload.
    if ([...state.files.values()].some(isBinaryValue)) {
      state.readBuffer = "";
      await sendLine("put --binary");
      const reply = await waitFor("badge> ", 5000);
      if (!reply.includes("PUT BINARY OK")) {
        throw new Error("badge firmware needs an update for image uploads; update firmware, then Push again");
      }
    }

    appendLog(`[push] slug=${slug}, ${state.files.size} files\n`, "info");

    await sendLine(`mkdir ${remoteDir}`);
    await waitFor("badge> ", 5000);

    const knownDirs = new Set([remoteDir]);
    const paths = [...state.files.keys()].filter(isPushable).sort();
    let i = 0;
    for (const path of paths) {
      i++;
      const remote = `${remoteDir}/${path}`;
      const parent = remote.substring(0, remote.lastIndexOf("/"));
      if (!knownDirs.has(parent)) {
        await sendLine(`mkdir ${parent}`);
        await waitFor("badge> ", 5000);
        knownDirs.add(parent);
      }
      const v = state.files.get(path) ?? "";
      const bytes = isBinaryValue(v) ? v : new TextEncoder().encode(v);
      setStatus(`pushing ${i}/${paths.length}: ${path} (${bytes.length} B)…`, "busy");
      appendLog(`[push] ${path} (${bytes.length} B)… `, "info");
      state.readBuffer = "";
      await sendLine(`put ${remote} ${bytes.length}`);
      pendingBytes = bytes.length;
      await waitFor("READY", 5000);
      await sendBytes(bytes);
      await waitFor(`OK ${bytes.length}`, 20000);
      pendingBytes = 0;
      appendLog("OK\n", "ok");
    }
    pendingBytes = 0;

    // The launcher only rescans /littlefs/apps at boot; without this the
    // pushed app never appears and the push looks like it "didn't work".
    state.readBuffer = "";
    await sendLine("reload");
    try {
      await waitFor("reload:", 8000);
      appendLog("[push] reload confirmed\n", "ok");
    } catch {
      appendLog("[push] reload not confirmed — reboot the badge if the app doesn't appear\n", "err");
    }

    setStatus(`pushed ${slug} (${paths.length} files)`, "ok");
  } catch (e) {
    appendLog("[push error] " + e.message + "\n", "err");
    const recovered = await resyncAfterPush(pendingBytes);
    pendingBytes = 0;
    setStatus(
      recovered
        ? `push failed (${e.message}) — badge recovered, safe to retry`
        : `push failed (${e.message}) — badge not responding, reboot it, reconnect, retry`,
      "err",
    );
  } finally {
    $("push").disabled = false;
    $("import-app").disabled = false;
  }
}

async function reboot() {
  if (!state.writer) return;
  await sendLine("reboot");
}

function manifestTextIcon() {
  const manifest = state.files.get("manifest.cfg");
  if (typeof manifest !== "string") return null;
  const m = manifest.match(/^\s*icon\s*=\s*(\S+)/m);
  return m ? m[1] : null;
}

function renderIconPreview() {
  const canvas = $("icon-preview");
  const meta = $("icon-meta");
  if (!canvas || !meta) return;
  const ctx = canvas.getContext("2d");
  const bin = state.files.get(ICON_BIN);
  const fallback = manifestTextIcon();
  const fallbackText = fallback ? `text fallback: ${fallback}` : "no manifest icon= yet";
  if (!isBinaryValue(bin)) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    meta.textContent = `no icon.bin\n${fallbackText}`;
    return;
  }
  const img = decodeIconBin(bin);
  if (!img) {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    meta.textContent = `icon.bin is corrupt (${bin.length} B)\n${fallbackText}`;
    return;
  }
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  ctx.putImageData(new ImageData(img.rgba, img.w, img.h), 0, 0);
  meta.textContent = `icon.bin — ${bin.length} B, overrides text icon\n${fallbackText}`;
}

async function handleIconFileSelect(file) {
  if (!file) return;
  try {
    setStatus("converting icon…", "busy");
    const bmp = await createImageBitmap(file);
    const bin = imageToIconBin(bmp);
    if (bmp.close) bmp.close();
    state.models.delete(ICON_BIN);
    state.files.set(ICON_BIN, bin);
    refreshSlug();
    rebuildFileList();
    renderIconPreview();
    persist();
    setStatus(`icon.bin ready (${bin.length} B)`, "ok");
  } catch (e) {
    setStatus("icon convert failed: " + (e && e.message ? e.message : e), "err");
  }
}

function removeIconBin() {
  if (!state.files.has(ICON_BIN)) return;
  if (!confirm("delete icon.bin (revert to the manifest text icon)?")) return;
  state.files.delete(ICON_BIN);
  const model = state.models.get(ICON_BIN);
  if (model) {
    model.dispose();
    state.models.delete(ICON_BIN);
  }
  if (state.currentPath === ICON_BIN) {
    state.currentPath = null;
    if (state.editor) state.editor.setModel(null);
  }
  rebuildFileList();
  renderIconPreview();
  persist();
}

function setupIconPanel() {
  const input = $("icon-file");
  const remove = $("icon-remove");
  if (input) {
    input.addEventListener("change", () => {
      void handleIconFileSelect(input.files[0]);
      input.value = "";
    });
  }
  if (remove) remove.onclick = removeIconBin;
  renderIconPreview();
}

window.addEventListener("DOMContentLoaded", () => {
  // Monaco loader sets up a global `require`. Point it at the CDN.
  // eslint-disable-next-line no-undef
  require.config({
    paths: { vs: "https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs" },
  });
  // eslint-disable-next-line no-undef
  require(["vs/editor/editor.main"], () => {
    state.editor = monaco.editor.create($("editor"), {
      theme: "vs-dark",
      automaticLayout: true,
      fontSize: 13,
      minimap: { enabled: false },
      tabSize: 2,
      insertSpaces: true,
    });

    if (!loadPersisted()) seedDefaults();
    refreshSlug();
    rebuildFileList();
    renderIconPreview();
    const first = [...state.files.keys()].sort()[0];
    if (first) openFile(first);
    $("import-app").disabled = false;
    $("download-app").disabled = false;
    void refreshGuide();
  });

  setupIconPanel();

  $("import-app").onclick = () => {
    previewAppImport();
    $("import-app-dialog").showModal();
  };
  $("cancel-app-import").onclick = () => $("import-app-dialog").close();
  $("apply-app-import").onclick = importApp;
  $("app-source").addEventListener("input", previewAppImport);
  $("app-file").addEventListener("change", async (event) => {
    const file = event.target.files[0];
    if (!file) return;
    try {
      if (file.size > 128 * 1024) throw new Error("Choose a single app file under 128 KiB.");
      $("app-source").value = await file.text();
      previewAppImport();
    } catch (e) {
      $("import-preview").textContent = e.message;
      $("apply-app-import").disabled = true;
    }
    event.target.value = "";
  });
  $("download-app").onclick = downloadApp;

  $("connect").onclick = () => (state.port ? disconnect() : openConnectGuide());
  $("cancel-connect-guide").onclick = () => $("connect-guide").close();
  $("choose-device").onclick = () => {
    $("connect-guide").close();
    void connect();
  };
  $("push").onclick = push;
  $("reboot").onclick = reboot;
  $("break").onclick = sendBreak;
  $("add-file").onclick = addFile;
  $("clear-log").onclick = () => {
    $("log").textContent = "";
  };

  const consoleInput = $("console-input");
  consoleInput.addEventListener("keydown", async (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      const line = consoleInput.value;
      consoleInput.value = "";
      if (line && state.history[state.history.length - 1] !== line) {
        state.history.push(line);
      }
      state.historyIdx = state.history.length;
      if (!state.writer) {
        appendLog("(not connected)\n", "err");
        return;
      }
      try {
        await sendLine(line);
      } catch (err) {
        appendLog("[send error] " + err.message + "\n", "err");
      }
    } else if (e.key === "ArrowUp") {
      if (state.history.length === 0) return;
      e.preventDefault();
      state.historyIdx = Math.max(0, state.historyIdx - 1);
      consoleInput.value = state.history[state.historyIdx] ?? "";
    } else if (e.key === "ArrowDown") {
      if (state.history.length === 0) return;
      e.preventDefault();
      state.historyIdx = Math.min(state.history.length, state.historyIdx + 1);
      consoleInput.value = state.history[state.historyIdx] ?? "";
    }
  });

  // Save on Cmd/Ctrl-S (just persists; push is a separate button).
  window.addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "s") {
      e.preventDefault();
      persist();
      setStatus("saved", "ok");
    }
  });

  setupSplitters();

  // Periodic save while editing — interval + on tab close, so we never
  // hammer localStorage on every keystroke but also never lose >2s of work.
  setInterval(persist, PERSIST_INTERVAL_MS);
  window.addEventListener("beforeunload", persist);
  window.addEventListener("pagehide", persist);
});

function setupSplitters() {
  const body = document.body;
  const saved = JSON.parse(localStorage.getItem(LAYOUT_KEY) || "{}");
  if (saved.asideW) body.style.setProperty("--aside-w", saved.asideW + "px");
  if (saved.footerH) body.style.setProperty("--footer-h", saved.footerH + "px");

  const persistLayout = () => {
    const cs = getComputedStyle(body);
    localStorage.setItem(LAYOUT_KEY, JSON.stringify({
      asideW: parseInt(cs.getPropertyValue("--aside-w"), 10),
      footerH: parseInt(cs.getPropertyValue("--footer-h"), 10),
    }));
  };

  const drag = (handle, onMove, dragClass) => {
    handle.addEventListener("mousedown", (e) => {
      e.preventDefault();
      handle.classList.add("dragging");
      body.classList.add(dragClass);
      const move = (ev) => onMove(ev);
      const up = () => {
        handle.classList.remove("dragging");
        body.classList.remove(dragClass);
        window.removeEventListener("mousemove", move);
        window.removeEventListener("mouseup", up);
        persistLayout();
      };
      window.addEventListener("mousemove", move);
      window.addEventListener("mouseup", up);
    });
  };

  drag($("vsplit"), (ev) => {
    const w = Math.max(140, Math.min(window.innerWidth - 240, ev.clientX));
    body.style.setProperty("--aside-w", w + "px");
  }, "dragging-col");

  drag($("hsplit"), (ev) => {
    // footer grows as mouse moves up; reserve space for header (44) + hint (~28).
    const hintH = $("hint").offsetHeight;
    const max = window.innerHeight - 44 - hintH - 120;
    const h = Math.max(80, Math.min(max, window.innerHeight - ev.clientY - hintH));
    body.style.setProperty("--footer-h", h + "px");
  }, "dragging-row");
}
