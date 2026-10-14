#pragma once

// BBX1 bulk-transfer sender: pushes a buffer to a sync station across ~90
// advertising frames.
//
// Selective repeat with a bitmap, not stop-and-wait. A per-frame round trip
// would cost ~200 ms each way and is where all the time would go; instead the
// badge sweeps the whole payload, asks once what landed, and repairs.
//
// The thing to keep in mind reading this: hal_radio_send() does not transmit.
// It overwrites a persistent advertisement that keeps repeating until it is
// overwritten again — a register, not a queue. Every frame therefore has to be
// *held* for a dwell long enough that an advertising event carries it and the
// station's scan catches it, and the peer's own register is the only
// acknowledgement channel there is.
//
// Driven from the owning app's on_tick(), not from the registry: hal_radio
// holds a single recv callback (hal_radio.cpp), so a registry-driven transport
// would have to own that one slot permanently and multiplex it to whichever
// app also wanted it. The app registers a lambda that forwards here, exactly
// as app_blindbox does.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bbx_proto.h"

enum class XferState : uint8_t {
    Idle,
    Opening,     // asking for the lease
    Backoff,     // denied or someone else holds it; waiting to retry
    Sweeping,    // first pass over every frame
    AwaitStat,   // holding OPEN|SWEEP_DONE, waiting to be told what landed
    Repairing,   // re-sending the frames the bitmap says are missing
    AwaitDone,   // station is doing TLS and the POST
    Done,
    Failed,
};

enum class XferFail : uint8_t {
    None,
    Radio,         // hal_radio wouldn't come up
    NoStation,     // nothing answered the OPEN
    StationBusy,   // denied too many times
    Lost,          // heard it, then stopped hearing it
    Stalled,       // repair rounds stopped making progress
    Crc,           // station reassembled it and the checksum disagreed
    TooBig,        // station refused the size
    Server,        // station got it and the upload failed; see server_status()
};

struct XferConfig {
    // Payload source: exactly one of data/file. RAM needs the whole buffer
    // up front; a file streams one frame at a time, so a transfer far larger
    // than the heap only pins a FILE* plus a frame on the tick stack.
    const uint8_t *data;   // caller-owned; must outlive the transfer
    FILE          *file;   // caller-owned, open "rb"; must outlive the transfer
    size_t         len;    // 1..BBX_XFER_MAX_LEN
    uint32_t       crc;    // checksum over the payload; computed from data,
                           // or caller-supplied when file != nullptr
    uint8_t        kind;   // BBX_XFER_KIND_*
    uint8_t        enc;    // BBX_XFER_ENC_*
    uint8_t       *response; // optional profile response buffer, includes six-byte wire prefix
    size_t         response_cap; // at least BBX_PROFILE_MAX_LEN + 7
};

struct XferStats {
    uint8_t  total_frames;
    uint8_t  recv_count;     // as of the last STAT
    uint8_t  repair_rounds;
    uint32_t frames_sent;    // including repairs
    uint32_t elapsed_ms;
    int8_t   station_rssi;
    char     station_id[BBX_MAX_BOX_ID + 1];
};

// Begins a transfer. Returns false if one is already running, the payload is
// empty or past BBX_XFER_MAX_LEN, or the radio is not enabled.
bool bbx_xfer_start(const XferConfig &cfg);

// Safe from any state. Restores the radio's default timings.
void bbx_xfer_abort();

// Drive from the app's on_tick(), i.e. every 20 ms.
void bbx_xfer_tick();

// Runs on the NimBLE host task. Copies into a single slot and returns; the
// real work happens in bbx_xfer_tick().
void bbx_xfer_on_frame(const uint8_t mac[6], int8_t rssi,
                       const uint8_t *data, size_t len);

XferState bbx_xfer_state();
XferFail  bbx_xfer_fail();

// BBX_ST_* from XFER_DONE. Only meaningful when fail() == Server.
uint8_t bbx_xfer_server_status();

// 0..100, weighted so the first sweep is most of the bar.
int bbx_xfer_progress_pct();

const XferStats *bbx_xfer_stats();

// Station discovery, usable while Idle. Returns false if no SYNC_BEACON has
// been heard recently. `out_id` may be empty if the station is unconfigured.
bool bbx_xfer_station_seen(char *out_id, size_t out_sz, int *rssi_out);

// Forget any heard station. Call from on_enter() so a stale sighting from a
// previous visit to the app isn't reported as current.
void bbx_xfer_reset_discovery();

// Only available after a successful sync with a profile response. Excludes the wire prefix.
const char *bbx_xfer_profile(size_t *len);
