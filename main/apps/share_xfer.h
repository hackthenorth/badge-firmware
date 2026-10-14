#pragma once

// App Share transfer engine: moves one app bundle between two badges.
//
// Free functions over file-static state, the shape of bbx_xfer / mp_session,
// for the reason bbx_xfer.h:17-21 gives: hal_radio holds a single recv
// callback, so the engine owns it while active and the app drives everything
// from on_tick() (20 ms).
//
// Sender: start_send() -> advertise OFFER until accepted or cancelled -> DATA sweep ->
// repair from STAT bitmap -> DONE.
// Receiver: start_listen() -> OFFER latches -> app shows a prompt -> user
// accepts -> accept_offer() -> ACCEPT
// burst -> collect DATA while holding STAT -> checksum verification -> Done with the bundle on disk.
//
// The app polls state()/progress()/offer_*()/rx_*() from on_tick() and owns
// all UI, install, and conflict decisions. This module never touches LVGL.

#include <stddef.h>
#include <stdint.h>

enum class ShareRole : uint8_t {
    Idle,
    Sender,
    Receiver,
};

enum class ShareState : uint8_t {
    Idle,
    // Sender.
    Offering,    // holding OFFER, waiting for ACCEPT
    Sweeping,    // first pass over every DATA frame
    Repairing,   // re-sending frames the STAT bitmap says are missing
    DoneHold,    // holding final DONE (sender) or verified STAT (receiver)
    // Receiver.
    Listening,   // scanning, saying nothing
    OfferReady,  // an offer latched; app is showing the accept prompt
    Accepting,   // holding ACCEPT immediately after user acceptance
    Receiving,   // collecting DATA into a temporary file, holding STAT
    Verifying,   // checking the file CRC in 1 KiB blocks
    // Terminal.
    Done,
    Failed,
};

enum class ShareFail : uint8_t {
    None,
    Radio,       // hal_radio wouldn't come up
    Declined,    // receiver's user declined (receiver side only)
    Cancelled,   // local user aborted, or sender sent DONE(CANCEL)
    Lost,        // heard a peer, then stopped hearing them
    Stalled,     // repair rounds stopped making progress
    Crc,         // reassembled bundle failed its checksum
    TooBig,      // offer exceeds the bundle cap
    NoSpace,     // insufficient RAM
    StorageFull, // filesystem explicitly reported ENOSPC
    Storage,     // staging file could not be opened/read/written
};

// Sender streams from a closed, pre-packed file. The caller owns its path
// and must keep it unchanged until abort(); CRC comes from pack_file().
// Start may return true with state Failed (inspect fail() for the reason).
bool share_xfer_start_send(const char *bundle_path, size_t len, uint32_t crc,
                           const char *slug, const char *app_name,
                           const char *version);

// Receiver owns this temporary path until abort(), which removes it.
// It must be a staging path, never an installed app file. Creates its parent
// directory if missing, so a badge with no installed apps can receive its first.
bool share_xfer_start_listen(const char *receive_path);

// Receiver user accepted the latched offer. Copies display_name for ACCEPT.
// Immediately starts reception; no bump or sender button press is required.
// False if there is no latched offer (e.g. it timed out). As with start_send(),
// true may indicate Failed: inspect state()/fail() for storage or radio errors.
bool share_xfer_accept_offer(const char *display_name);

// Safe from any state. Clears the recv callback first (NimBLE task), pauses
// advertising, restores radio timings. Does NOT hal_radio_disable(): the
// registry does that on the next app switch anyway.
void share_xfer_abort();

// Drive from the app's on_tick(), i.e. every 20 ms.
void share_xfer_tick();

// Install as the radio recv callback while the engine runs. Runs on the
// NimBLE host task: copies into a small queue and returns; decoding happens
// in share_xfer_tick().
void share_xfer_on_frame(const uint8_t mac[6], int8_t rssi,
                         const uint8_t *data, size_t len);

ShareRole  share_xfer_role();
ShareState share_xfer_state();
ShareFail  share_xfer_fail();

// 0..100. Sender: sweep owns 0..80, repair 80..99. Receiver: bitmap fill.
int share_xfer_progress_pct();

// Latched offer for the receiver's accept prompt. False when none is
// latched (Listening, or after accept/timeout). RSSI is the offer's signal;
// 127 means unavailable.
bool share_xfer_offer(char *slug_out, size_t slug_sz,
                      char *name_out, size_t name_sz,
                      char *ver_out, size_t ver_sz,
                      size_t *bundle_len_out, uint32_t *crc_out,
                      int *rssi_out);

// Received bundle, valid only while state() == Done on the receiver. The
// engine owns the temporary file: unpack synchronously, then abort().
const char *share_xfer_rx_file(size_t *len_out);
