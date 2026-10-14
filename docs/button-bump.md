# Connect with the A button

Open Connect on both badges and hold them within about 10 cm. Tap A on both
for at least one second of discovery, or hold A to keep discovering until
release, up to two seconds. Both badges need compatible button-pairing
firmware. Physical bumping still works
with the existing BMP2 protocol. B/Start opens saved contacts; A in the
contacts list still opens details.

The button flow uses `BPA1` frames containing a fresh attempt nonce, the
chosen peer's nonce as acknowledgement, and the existing BMP2 contact-card
payload. The entire frame remains at most 225 bytes. Older firmware ignores
it, and the button flow ignores physical-bump frames.

Each badge must receive its peer's attempt **after its own press and inside
its 1-2 second acceptance window**, measured with its local monotonic clock. RX timestamps are captured
in the radio callback so delayed UI processing cannot make an old frame fresh.
The first eligible peer MAC and nonce are fixed for that attempt. A contact
is saved only after that peer acknowledges the local nonce. This needs no
shared clock. A matching ACK first arriving during the bounded transport tail
can also confirm the exchange, since it proves the peer heard that fresh nonce.

Unmatched advertisements stop when the acceptance window ends; a selected
pair can repeat acknowledgements for up to another 500 ms. Neither repeated
Pressed events nor an ACK extends those deadlines. Advertising is paused
on the next app tick after a deadline and immediately on leaving Connect.
Connect keeps its receiver off at idle. Both triggers use 30 ms advertising and
continuous scanning. Physical bumps transmit and listen for one full second,
even after an ACK, without extending the window on repeated impacts. The BLE
controller repeats the payload continuously; failed sends/resumes are retried
every 100 ms. RX uses a bounded, allocation-free queue. RF switches off when
the attempt ends.
Physical detection requires a combined-axis acceleration change above 400 mg;
smaller movements stay quiet. Unprovisioned badges stay silent. Both pairing
paths reject unavailable RSSI readings and signals below -64 dBm. This tighter
filter supports the 10 cm guidance, but does not measure distance; actual range
must be checked on two badges in their normal orientations.
The timing handshake is not authentication or encryption: contact cards still
use the existing short, cleartext broadcast and unique-contact store.

New contacts show green confirmation. Existing contacts show **Already
connected** with yellow LEDs. Both results last at least three seconds.
A failed attempt shows **No
badge found, try again** in red for two seconds. Leaving Connect clears the LEDs.

Validation:

- `make -C tools/button-bump-test check`: boundary times, independent clocks,
  mutual ACK, stale attempts, peer selection, expiry, and frame parsing, under
  AddressSanitizer and UndefinedBehaviorSanitizer.
- `make -C tools/button-pair-test check`: tap minimum, held-button release,
  two-second cap, packet loss, and unchanged fixed-window callers.
- `python3 tools/badge-qa/run.py --out-dir /tmp/button-bump-qa`: real Connect UI,
  button dispatch, radio queue, contact persistence, and physical-bump regression.
- On two flashed badges, repeat tap/hold flows with staggered starts,
  check both saved lists, try only one press, then sync to confirm networking
  credit. Host tests simulate packet timing; actual RF reliability needs this
  two-badge check.

Chess retains a fixed one-second gate with a separate `BPI1` intent frame.
The frame binds the attempt to Chess and its selected peer. Each side
acknowledges the chosen peer and holds the exchange for at most 1.5 seconds
before resuming MPG1. Both open Chess then bump or both press A within one
second; B selects pass-and-play. This adds intent, not authenticated pairing.

Share uses APP1 offer/accept directly. The sender picks an app and presses
**A: offer app** on its details screen, then keeps advertising until accepted
or cancelled. The receiver reviews **Incoming app** and presses **A: accept**
once to start reception. There is no bump, second sender press, or BPI1 gate.
An unaccepted receiver stays silent and never writes incoming DATA. Transfers
bind to the offer ID and peer MAC, and verify the whole bundle before install;
this is not authentication or encryption. Update both badges if either still
shows the old bump/confirmation instructions.
