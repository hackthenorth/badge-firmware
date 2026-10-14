#pragma once
#include "share_xfer.h"
struct Engine {
    decltype(&share_xfer_start_send) send;
    decltype(&share_xfer_start_listen) listen;
    decltype(&share_xfer_accept_offer) accept;
    decltype(&share_xfer_tick) tick;
    decltype(&share_xfer_abort) abort;
    decltype(&share_xfer_on_frame) receive;
    decltype(&share_xfer_state) state;
    decltype(&share_xfer_fail) fail;
    decltype(&share_xfer_rx_file) file;
};
Engine sender_engine();
Engine receiver_engine();
