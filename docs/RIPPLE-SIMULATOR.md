# Ripple simulator

Ripple is a deliberately non-critical propagation experiment layered onto a
successful Badge Bump. It never decides whether networking succeeds and never
changes contact data.

## State and propagation

- A badge without Ripple has no active state.
- An origin badge starts at depth 0.
- The first successful bump with an active peer stores `peer depth + 1`.
- Once active, a badge keeps its first depth. Later encounters do not rewrite
  its history or turn it off.
- Depth saturates at 255.

The state is stored as:

```text
/littlefs/config/ripple.cfg

depth=2
```

An explicit test clear stores `depth=-1`. A badge that has never participated
has no Ripple config file. Both forms are easy to distinguish in the ordinary
filesystem snapshot, which already walks `config` before lower-priority data.

## Badge Bump wire format

Active badges add one optional TLV to the existing BMP2 card:

```text
tag=10, length=1, value=<depth>
```

Unknown TLVs are already skipped, so older firmware continues networking
normally. The extra record is only three bytes and is sent inside the same
short, acknowledgement-gated bump window as the contact card.

Ripple observes a contact only after `contacts_store_save()` succeeds. Any
Ripple read, write, or decode failure leaves the contact exchange untouched.
The raw contact file also records `ripple_depth=<n>` (or `-1` when absent), so
snapshot analysis can identify which successful encounters carried Ripple.
This field is intentionally omitted from the curated Lua contacts API.

## QR marker

Only the QR view of My Badge shows Ripple. An active badge shows its numeric
depth in small grey text at the bottom-right corner, with no box. The number is
outside the QR image so it cannot interfere with scanning. It does not appear
on the details page or elsewhere in the launcher.

## Seeding and testing

The USB console supports:

```text
ripple             # print the command that recreates the current state
ripple seed        # make this badge an origin at depth 0
ripple set 4       # set an explicit test depth
ripple clear       # mark this badge inactive
```

Seed any number of origin badges independently. No seed selection, analytics,
or event logic is load-bearing firmware behavior; analysis happens later from
the uploaded filesystem snapshots.
