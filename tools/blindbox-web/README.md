# Solana Scavenger Hunt Web Serial POC

A dependency-free browser bridge for the badge's Solana Scavenger Hunt `WEB` mode.

## Run it

Web Serial requires Chrome or Edge on desktop and a secure context. Serve this
directory on localhost:

```sh
python3 -m http.server 8000 --directory tools/blindbox-web
```

Then open <http://localhost:8000>, leave the POC box slug as `blind-box-01`,
and connect the badge's **USB JTAG/serial debug unit**. Open Solana Scavenger Hunt on the
badge and press A while its mode label says `WEB`.

For the built-in mock backend, run the combined static + API server instead:

```sh
node tools/blindbox-web/server.mjs
```

The page defaults its endpoint to <http://localhost:8000/api/open>. The mock
validates the four request fields and responds with a greeting plus a real
Solana Explorer devnet URL. It never submits a transaction or changes chain
state. Replace the endpoint field when the real backend is available.

The POST endpoint must allow the page's origin through CORS and accept:

```json
{
  "box": "blind-box-01",
  "user_id": "123456",
  "name": "Ada Lovelace",
  "email": "ada@example.com"
}
```

It must return HTTP 2xx with:

```json
{
  "message": "Hello world!",
  "url": "https://explorer.solana.com/..."
}
```

## Serial protocol

The badge emits one marked line. The box slug is intentionally absent because
the browser represents the box and adds it to the HTTP body:

```text
BBX_WEB_REQUEST <request_id> {"user_id":"123456","name":"Ada Lovelace","email":"ada@example.com"}
```

The page returns the server JSON without command-line escaping:

```text
blindbox_response <request_id> <byte_count>\r
BBX_WEB_READY <request_id>
<exactly byte_count raw JSON bytes>
BBX_WEB_ACCEPTED <request_id>
```

Bodies are capped at 512 bytes and written in 128-byte chunks with a short
pause. This is required because the badge's USB-Serial-JTAG RX ring is 256
bytes. A request id prevents a slow response from being applied to a later
bump after the badge times out.

Incomplete serial response bodies time out after 10 seconds, returning control
to the console. Browser HTTP requests time out after 20 seconds.
