import { buildSync } from "esbuild";
import { fileURLToPath } from "node:url";

// Compile the actual transport with the same build-time env replacement as
// Vite, so tests can prove production methods fail before touching USB.
export async function serialBuild(env) {
  const result = buildSync({
    entryPoints: [
      fileURLToPath(new URL("../../src/sync/serial.js", import.meta.url)),
    ],
    bundle: true,
    format: "esm",
    platform: "browser",
    write: false,
    define: { "import.meta.env": JSON.stringify(env) },
  });
  return import(
    "data:text/javascript;base64," +
      Buffer.from(result.outputFiles[0].text).toString("base64")
  );
}
