// A valid Lua file with a manifest in its leading long comment. No code is
// evaluated here; the badge remains responsible for compiling/running Lua.
globalThis.BadgeAppBundle = (() => {
  const OPEN = "--[==[badge-app";
  const CLOSE = "]==]";
  const bytes = (s) => new TextEncoder().encode(s).length;

  function validateManifest(text) {
    const fields = Object.create(null);
    const allowed = new Set([
      "slug", "name", "icon", "api", "heap_kb", "wake_lock",
      "home_button", "back_button", "confirm_home", "version", "author",
    ]);
    for (const raw of text.split("\n")) {
      if (bytes(raw) > 254) throw new Error("Manifest lines must be under 255 bytes.");
      const line = raw.replace(/^[ \t]+|[ \t]+$/g, "");
      if (!line || line.startsWith("#")) continue;
      const match = line.match(/^([^=#]+)=([^=#]*)$/);
      if (!match) throw new Error("Use key=value, with comments on separate lines.");
      const trim = s => s.replace(/^[ \t]+|[ \t]+$/g, "");
      const key = trim(match[1]), value = trim(match[2]);
      if (!allowed.has(key)) throw new Error(`Unknown manifest key: ${key}`);
      if (Object.hasOwn(fields, key)) throw new Error(`Duplicate manifest key: ${key}`);
      fields[key] = value;
    }
    if (!/^[a-z0-9][a-z0-9_-]{0,31}$/.test(fields.slug || "")) {
      throw new Error("slug must be 1–32 lowercase letters/digits, _ or -, starting with a letter/digit.");
    }
    if (!fields.name || bytes(fields.name) > 48) throw new Error("name must be 1–48 UTF-8 bytes.");
    if (fields.icon !== undefined && (!fields.icon || bytes(fields.icon) > 12)) {
      throw new Error("icon must be 1–12 UTF-8 bytes.");
    }
    for (const key of ["version", "author"]) {
      if (fields[key] !== undefined && bytes(fields[key]) > 48) throw new Error(`${key} must be at most 48 bytes.`);
    }
    const choices = {
      api: ["1", "2"], heap_kb: ["48", "96"], wake_lock: ["0", "1"],
      home_button: ["0", "1"], back_button: ["0", "1"], confirm_home: ["0", "1"],
    };
    for (const [key, values] of Object.entries(choices)) {
      if (fields[key] !== undefined && !values.includes(fields[key])) {
        throw new Error(`${key} must be ${values.join(" or ")}.`);
      }
    }
    if (fields.confirm_home === "1" && (fields.home_button ?? fields.back_button) === "1") {
      throw new Error("confirm_home=1 cannot be combined with home_button=1.");
    }
    return fields;
  }

  function parse(input) {
    let source = input.replace(/^\uFEFF/, "").replace(/\r\n/g, "\n").trim();
    // Accept the agent's one fenced code block as well as a raw .lua file.
    const fenced = source.match(/^```(?:lua)?[ \t]*\n([\s\S]*?)\n```$/);
    if (fenced) source = fenced[1];
    if (/^[ \t]*```/m.test(source)) throw new Error("Paste one complete app, not multiple Markdown code blocks.");
    if (/[\0\r]/.test(source)) throw new Error("Use LF or CRLF text without NUL bytes.");
    if (!source.startsWith(OPEN + "\n")) throw new Error(`Start the file with ${OPEN} on its own line.`);
    const end = source.indexOf("\n" + CLOSE + "\n", OPEN.length);
    if (end < 0) throw new Error(`End the manifest with ${CLOSE} on its own line, then include the full Lua code.`);
    const manifest = source.slice(OPEN.length + 1, end) + "\n";
    if (manifest.includes(CLOSE)) throw new Error("The manifest contains a Lua comment delimiter.");
    const fields = validateManifest(manifest);
    const main = source.slice(end + CLOSE.length + 2).replace(/^\n+/, "") + "\n";
    if (!main.trim()) throw new Error("Include the full Lua app after the manifest.");
    if (bytes(main) > 64 * 1024) throw new Error("main.lua must be at most 64 KiB.");
    return { slug: fields.slug, manifest, main };
  }

  function serialize(manifest, main) {
    const source = `${OPEN}\n${manifest.trim()}\n${CLOSE}\n\n${main.trim()}\n`;
    parse(source);
    return source;
  }

  return { parse, serialize };
})();
