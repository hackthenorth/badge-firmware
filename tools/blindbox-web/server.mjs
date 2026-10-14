import { createReadStream } from "node:fs";
import { stat } from "node:fs/promises";
import { createServer } from "node:http";
import { extname, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(fileURLToPath(new URL("./", import.meta.url)));
const port = Number.parseInt(process.env.PORT || "8000", 10);
const maxBodyBytes = 16 * 1024;

const contentTypes = {
  ".html": "text/html; charset=utf-8",
  ".md": "text/markdown; charset=utf-8",
};

function sendJson(response, status, body) {
  const json = JSON.stringify(body);
  response.writeHead(status, {
    "access-control-allow-origin": "*",
    "cache-control": "no-store",
    "content-length": Buffer.byteLength(json),
    "content-type": "application/json; charset=utf-8",
  });
  response.end(json);
}

async function readJson(request) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > maxBodyBytes) throw new Error("request body is too large");
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8"));
}

function validString(value) {
  return typeof value === "string" && value.trim().length > 0;
}

async function handleMockOpen(request, response) {
  let body;
  try {
    body = await readJson(request);
  } catch (error) {
    sendJson(response, 400, { error: `invalid JSON: ${error.message}` });
    return;
  }

  const required = ["box", "user_id", "name", "email"];
  const missing = required.filter((key) => !validString(body?.[key]));
  if (missing.length > 0) {
    sendJson(response, 400, { error: `missing fields: ${missing.join(", ")}` });
    return;
  }

  console.log(
    `[mock] box=${body.box} user_id=${body.user_id} ` +
    `name=${JSON.stringify(body.name)} email=${body.email}`,
  );

  // A real Solana URL, but only to the devnet System Program account. This
  // mock never submits a transaction or changes blockchain state.
  sendJson(response, 200, {
    message: `Hello ${body.name}! You bumped ${body.box}.`,
    url: "https://explorer.solana.com/address/11111111111111111111111111111111?cluster=devnet",
  });
}

async function serveStatic(pathname, response, headOnly = false) {
  let relative;
  try {
    relative = pathname === "/" ? "index.html" : decodeURIComponent(pathname.slice(1));
  } catch {
    response.writeHead(400).end("Malformed URL");
    return;
  }
  const filePath = resolve(root, relative);
  if (!filePath.startsWith(`${root}${sep}`)) {
    response.writeHead(403).end("Forbidden");
    return;
  }

  try {
    const info = await stat(filePath);
    if (!info.isFile()) throw new Error("not a file");
    response.writeHead(200, {
      "cache-control": "no-store",
      "content-length": info.size,
      "content-type": contentTypes[extname(filePath)] || "application/octet-stream",
    });
    if (headOnly) response.end();
    else createReadStream(filePath).pipe(response);
  } catch {
    response.writeHead(404, { "content-type": "text/plain; charset=utf-8" });
    response.end("Not found");
  }
}

const server = createServer(async (request, response) => {
  let url;
  try {
    url = new URL(request.url || "/", "http://localhost");
  } catch {
    response.writeHead(400).end("Malformed URL");
    return;
  }

  if (request.method === "OPTIONS" && url.pathname === "/api/open") {
    response.writeHead(204, {
      "access-control-allow-headers": "content-type",
      "access-control-allow-methods": "POST, OPTIONS",
      "access-control-allow-origin": "*",
    });
    response.end();
    return;
  }

  if (request.method === "POST" && url.pathname === "/api/open") {
    await handleMockOpen(request, response);
    return;
  }

  if (request.method === "GET" || request.method === "HEAD") {
    await serveStatic(url.pathname, response, request.method === "HEAD");
    return;
  }

  response.writeHead(405, { allow: "GET, HEAD, POST, OPTIONS" });
  response.end("Method not allowed");
});

server.listen(port, "127.0.0.1", () => {
  console.log(`Solana Scavenger Hunt mock server: http://localhost:${port}`);
  console.log(`Mock POST endpoint:    http://localhost:${port}/api/open`);
});
