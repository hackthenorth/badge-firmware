# Snapshot Parse Test

Single static page (`index.html`, no build) that replicates the sync page's
`readUpload()` capture logic from `my.hackthenorth.com`
(`badgeProvisioner.ts`) in isolation: `badge_token get` (5 s), `snapshot`
(30 s), first-`{`-line + `JSON.parse`, then the token/size gate — with the
same stage strings and user-facing error.

Unlike the site, it also prints a content-safe structure map (line number,
length, tag per physical line) plus the `bytes=`/`crc32=` trailer versus the
captured line, so a torn capture (interleaved logs, panic dump, early prompt
match) is visible instead of collapsing into "Could not read a safe snapshot".

Nothing is uploaded; nothing is written to the badge.

## Serve it

Web Serial needs a secure context, so open it over `localhost`, not `file://`:

```sh
cd tools/snapshot-parse-test
python3 -m http.server 8000
# open http://localhost:8000 in desktop Chrome/Edge
```

Click **Connect badge** (pick *USB JTAG/serial debug unit*), then
**Run site logic**.
