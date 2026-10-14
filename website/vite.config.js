import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import { createReadStream, statSync } from "node:fs";
import { dirname, extname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const websiteDirectory = dirname(fileURLToPath(import.meta.url));
const ideDirectory = resolve(websiteDirectory, "../tools/badgectl/web");
const contentTypes = {
  ".css": "text/css; charset=utf-8",
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
};

function cacheCloseupPhotosInDevelopment() {
  return {
    name: "cache-closeup-photos-in-development",
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const pathname = new URL(request.url, "http://localhost").pathname;
        if (/^\/closeup\/badge-(front|back)\.[a-f0-9]{12}\.png$/.test(pathname)) {
          try {
            if (statSync(resolve(websiteDirectory, "public", pathname.slice(1))).isFile()) {
              response.setHeader("Cache-Control", "public, max-age=31536000, immutable");
            }
          } catch {
            // Let Vite handle missing files without caching its fallback response.
          }
        }
        next();
      });
    },
  };
}

function serveBadgeIdeInDevelopment() {
  return {
    name: "serve-badge-ide-in-development",
    configureServer(server) {
      server.middlewares.use((request, response, next) => {
        const pathname = new URL(request.url, "http://localhost").pathname;

        if (pathname === "/ide") {
          response.statusCode = 302;
          response.setHeader("Location", "/ide/");
          response.end();
          return;
        }

        if (!pathname.startsWith("/ide/")) {
          next();
          return;
        }

        const relativePath = decodeURIComponent(pathname.slice("/ide/".length)) || "index.html";
        const filePath = resolve(ideDirectory, relativePath);
        if (!filePath.startsWith(`${ideDirectory}${sep}`)) {
          response.statusCode = 403;
          response.end("Forbidden");
          return;
        }

        try {
          if (!statSync(filePath).isFile()) {
            next();
            return;
          }
        } catch {
          next();
          return;
        }

        response.statusCode = 200;
        response.setHeader("Content-Type", contentTypes[extname(filePath)] || "application/octet-stream");
        response.setHeader("Cache-Control", "no-store");
        createReadStream(filePath).pipe(response);
      });
    },
  };
}

export default defineConfig({
  plugins: [cacheCloseupPhotosInDevelopment(), serveBadgeIdeInDevelopment(), react()],
});
