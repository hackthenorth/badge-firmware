# Solana Scavenger Hunt webhook spec

This is the contract the quest server must speak. The badge builds the
request, a BLE relay POSTs it here verbatim, and the response travels back
over BLE to the badge. The relay never parses either direction.

Body budget: the response must fit one BLE advertising frame —
**BBX_MAX_BODY (207) bytes** including JSON syntax. The relay truncates
anything longer and the badge shows "Empty box?" instead of an item.

## Request

`POST <RELAY_WEBHOOK_URL>` - `Content-Type: application/json`

```json
{
  "box": "blind-box-01",
  "user_id": "123456",
  "name": "Ada Lovelace",
  "email": "ada@example.com",
  "public_key": "5oNDL3swdJJF1g9DzJiZ4ynHXgszjAEpUkxVYejchzrY"
}
```

| Field | Meaning |
|---|---|
| `box` | Station id from the relay's beacon — which box the attendee walked up to. |
| `user_id` | HTN attendee id as a string. Deliberately not the BLE MAC. |
| `name` / `email` | Badge identity (`display_name`, `account_email`). |
| `public_key` | Attendee's Solana wallet, base58 ed25519. The badge never signs — the server mints to this key. |

## Response

`200` - JSON object.

```json
{
  "new_item": "1",
  "chain_link": "https://quest.example.com/c/abc123",
  "all_items": ["1", "3", "5"]
}
```

| Field | Meaning |
|---|---|
| `new_item` | Item id won at this box. Empty/missing renders as "Empty box?". Keep ids strings (`"1"`, not `1`) — future non-numeric ids must not break parsing. `"0"` is reserved: it means "no pal here — finish on your phone", and the badge shows its final-quest message plus the saved link QR instead of a reveal. Never include `"0"` in `all_items`. |
| `chain_link` | URL behind the badge's B-button QR: the quest console on the first tap, a chain-explorer transaction later. Empty string hides the QR. |
| `all_items` | **Authoritative inventory, including the new item.** The badge replaces its local state wholesale — a lost reply resyncs on the next box, so never send a delta. |

Item ids map to pal icons through a compile-time table on the badge with
slots for all 9 rewards; unknown ids render a generic placeholder. The server
may add rewards after the firmware freeze.
