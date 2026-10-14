# Share streaming tests

Run `make check` here. The codec tests use real files; `test-engine.py` compiles
sender and receiver copies of the real transfer engine with simulated BLE
advertising registers, clock, and mutexes. The engine suite uses AddressSanitizer
and UndefinedBehaviorSanitizer by default (`SHARE_TEST_FLAGS` overrides flags).

Coverage includes 1 KiB boundaries and the 48 KiB bundle cap, byte-for-byte
compatibility with APPB v1, incremental CRC, safe unpacking, missing files,
truncation, dropped/duplicate/delayed radio frames, peer binding, malformed
metadata, final acknowledgement loss, short IO, source read failure, full
receiver storage versus RAM exhaustion or file-access errors, first-app reception
with no apps directory, one-press acceptance, persistent offers, ignoring
unaccepted DATA, cancellation, and a lost sender's timeout. Every transfer read
and write is checked to stay at or below 1 KiB.

Firmware stages bundles in `/littlefs/apps/.share-tx.bin` and
`/littlefs/apps/.share-rx.bin`. Packing, verification, and unpacking use a
1024-byte scratch buffer; DATA remains 213 bytes inside the existing 225-byte
APP1 radio frame. The frame bitmap drives retries from the file. A full
acknowledgement is advertised only after CRC verification and a successful
file close. The receive file stays available until installation consumes it
and calls `share_xfer_abort()`.

Host tests do not establish ESP32 heap availability, flash wear, or real-radio
performance. Hardware validation requires updated firmware and two badges.

Device smoke check (2026-09-16): flashed an ESP32-C3 badge, staged the 7,482-byte
Connect 4 bundle, reached the offer screen, cancelled and verified temporary
file removal, repeated the send without rebooting, and entered receive mode.
All passed. Receive mode reported 8,792 bytes free heap, with a 7,680-byte
largest block. A fresh-badge receive check also confirmed that entering Receive creates
`/littlefs/apps` when it is absent. The first mixed-firmware attempt stalled
at 2%. After updating the sender, a live 7,482-byte Connect 4 transfer completed
with one receiver acceptance: the sender reported Sent (the receiver had
acknowledged the verified CRC), and the user confirmed installation on the
other badge. This validates that transfer, not every RF condition or app size.

Sync and Share now request one fresh boot when entered from the launcher.
Both entry paths were exercised on the updated sender; Sync reached station
discovery and Share advertised Connect 4 without a radio error. The Share
screen reads "Accept on the receiving badge"; offering has no timeout and
ends only on acceptance, cancellation, or a radio error. Host tests hold an
unaccepted offer for over a simulated minute and verify that no DATA is sent.
Registry tests cover NFC/radio entry, direct app handoffs, and the one-shot
boot bypass.

This check also exposed controller advertising allocation failure after stack
initialization (`BLE_ERR_MEM_CAPACITY`, HCI error 519). The firmware defaults now
reserve eight MSYS_2 blocks and twelve high-priority HCI event buffers, retaining
all buffer sizes and eight scan-report event buffers. Older generated
`sdkconfig` files must use those counts too; changing defaults alone does not
replace values already present in an existing configuration.
