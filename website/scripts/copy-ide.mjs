import { cp, mkdir, rm } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const websiteDirectory = resolve(scriptDirectory, "..");
const source = resolve(websiteDirectory, "../tools/badgectl/web");
const destination = resolve(websiteDirectory, "dist/ide");

await rm(destination, { force: true, recursive: true });
await mkdir(destination, { recursive: true });
await cp(source, destination, {
  recursive: true,
  filter: (path) => !path.split("/").includes(".vite"),
});

console.log("Copied tools/badgectl/web to dist/ide");
