# Badge Snapshot Viewer

Single static page (`index.html`, no build) that fetches the badge's
`snapshot` console command over USB (Web Serial) and prints the filesystem
JSON. This is the same `fs_mirror` document the Sync app uploads over BLE —
here pulled over USB while plugged in, as a stepping stone to POSTing it to
the sync endpoint.

## Hosted viewer

The viewer is published at
<https://hackathon.github.io/badge-firmware/sync-viewer/>.

## Serve it

Web Serial needs a secure context, so open it over `localhost`, not `file://`:

```sh
cd tools/sync-viewer
python3 -m http.server 8000
# open http://localhost:8000 in desktop Chrome/Edge
```

Click **Connect badge** (pick *USB JTAG/serial debug unit*), then
**Fetch snapshot**. The full pretty-printed document appears on the page.

## Protocol

Sends `snapshot` + bare CR down the esp_console REPL (`main/hal/hal_console.cpp`),
reads until the next `badge> ` prompt, drops `ESP_LOG` chatter, and parses the
first `{…}` line. No firmware change was needed: the `snapshot` command already
emits exactly this document.
