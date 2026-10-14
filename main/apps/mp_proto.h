#pragma once

#include "chess_rules.h"

#include <cstddef>
#include <cstdint>

// Badge-to-badge multiplayer wire format ("MPG1").
//
// Rides the shared hal_radio manufacturer-data channel (company 0xFFFF)
// alongside Badge Bump's "BMP2" and Solana Scavenger Hunt's "BBX1" frames — the 4-byte
// magic below is what tells them apart. Every listener on that channel sees
// every frame, so always check the magic before touching anything else.
//
// THE ONE THING TO UNDERSTAND FIRST: hal_radio_send() is not a transmit. It
// overwrites a *persistent* advertisement that keeps repeating at the adv
// interval until it is overwritten or paused (hal_radio.h:54-57). A peer's
// frame is therefore a value, not a message: reading it twice does not mean it
// was sent twice, and reading it immediately after changing your own does not
// mean the peer has reacted yet. Everything here is designed around that.
//
// Two consequences shape the whole format:
//
//   * Every frame carries the FULL game state, not a move. A packed chess
//     position is 38 bytes and the whole frame is 82 of the 225 available, so
//     there is no reason to send deltas. The radio is a register rather than a
//     queue, so a lost incremental move would need sequence numbers, a receive
//     bitmap and gap-fill — all of bbx_xfer's selective-repeat machinery. With
//     full state a lost frame costs latency and never correctness, idempotence
//     is free (compare seq+sum), and a badge that left and came back re-adopts
//     from a single frame instead of needing a separate resync path.
//
//   * There is only ONE advertisement slot, so a frame cannot be "mine" or "an
//     ack of yours" — it must be both. Hence seq/sum (what I assert) and
//     ack_seq/ack_sum (what I have adopted from you) side by side in every
//     frame. There is no ACK frame type, and every ack doubles as a resync.
//
// DO NOT PAD FRAMES. Extended-advertising airtime scales with payload: an
// 82-byte frame's AUX_ADV_IND is ~840 us where a 225-byte one is ~1.9 ms, and
// at the 30 ms interval used while pushing a move that difference is worth
// roughly 2.5 mA. Frame size is a power lever here, not just headroom.
//
// Badge-only and C++-only, so this uses plain static_assert. Unlike
// bbx_proto.h there is exactly one copy of this file — the relay and station
// firmwares do not speak MPG1 — so there is no drift-checking script to keep
// in sync.

#define MPG_MAGIC_0 'M'
#define MPG_MAGIC_1 'P'
#define MPG_MAGIC_2 'G'
#define MPG_MAGIC_3 '1'
#define MPG_VERSION 1

// Frame size is dictated by the radio, not chosen. Two HCI ceilings apply and
// the smaller one wins; see hal_radio.h:22-47 for the derivation. Briefly:
// TX is 251 (one HCI Set Extended Advertising Data command) and RX is 229 (one
// Extended Advertising Report, which NimBLE does not reassemble past). RX
// binds, and the AD wrapper costs 4 bytes, so 225.
//
// Both failure modes look exactly like weak signal — the frames that vanish
// are the big ones, which is also what path loss does.
#define MPG_AD_OVERHEAD      4
#define MPG_HCI_MAX_ADV_DATA 251
#define MPG_HCI_MAX_ADV_RPT  229
#define MPG_MAX_FRAME        225   // == kHalRadioMaxPayload
#define MPG_HDR_SIZE         38
#define MPG_MAX_BODY         (MPG_MAX_FRAME - MPG_HDR_SIZE)   // 187

// Frame types.
#define MPG_TYPE_HELLO 0x01   // pairing beacon; body = mpg_hello_t
#define MPG_TYPE_STATE 0x02   // session + game state; body is game-specific
#define MPG_TYPE_BYE   0x03   // leaving cleanly; body_len == 0

// Games. A bitmask position in mpg_hello_t::games, and a value in hdr::game.
#define MPG_GAME_CHESS 0x01

// Session state, mirrored from MpState so a peer can see where we are without
// inferring it. PROPOSE means "I have picked you"; LIVE means "we are playing".
#define MPG_STATE_IDLE    0
#define MPG_STATE_SEEKING 1
#define MPG_STATE_PROPOSE 2
#define MPG_STATE_LIVE    3
#define MPG_STATE_OVER    4
#define MPG_STATE_REMATCH 5

// Header flags.
//
// MPG_FLAG_BUMPING is load-bearing and easy to mistake for redundant. Badge
// Bump can infer mutuality from "I heard anything at all inside my bump
// window", because it only transmits after a bump. A pairing HELLO has to be
// advertised continuously to be discoverable, so that same test would match
// any badge idling in the app across the room. The sender therefore has to say
// whether IT is inside its own bump window, and mutual bump means both.
//
// MPG_FLAG_URGENT is how a resigning or leaving badge pulls a peer that is
// scanning at 3.3% duty back up to speed, so an abandonment lands in a second
// rather than whenever the opponent next looks.
#define MPG_FLAG_BUMPING    (1 << 0)
#define MPG_FLAG_DRAW_OFFER (1 << 1)
#define MPG_FLAG_RESIGN     (1 << 2)
#define MPG_FLAG_REMATCH    (1 << 3)
#define MPG_FLAG_URGENT     (1 << 4)

// Every uint16_t sits at an even offset and `session` is 4-aligned, so frames
// memcpy straight to and from the wire on the little-endian C3 with no
// serialisation step — the same property bbx_proto.h:121-122 relies on.
typedef struct __attribute__((packed)) {
    char     magic[4];   //  0  'M','P','G','1' — check FIRST, the channel is shared
    uint8_t  ver;        //  4  MPG_VERSION
    uint8_t  type;       //  5  MPG_TYPE_*
    uint8_t  game;       //  6  MPG_GAME_*
    uint8_t  flags;      //  7  MPG_FLAG_*
    uint32_t session;    //  8  my_nonce ^ peer_nonce, LE. Never 0 once paired.
    uint8_t  src[6];     // 12  sender's BLE static random address
    uint8_t  dst[6];     // 18  intended peer; all-zero while unpaired
    uint8_t  state;      // 24  MPG_STATE_*
    uint8_t  seat;       // 25  0 = first mover (White), 1 = second
    uint8_t  game_no;    // 26  rematch counter inside the session; starts at 0
    uint8_t  rsv;        // 27  zero — the v2 growth slot
    uint16_t seq;        // 28  sender's state counter (chess: ply), LE
    uint16_t sum;        // 30  CRC-16/CCITT-FALSE over the body, LE
    uint16_t ack_seq;    // 32  peer seq the sender has adopted, LE
    uint16_t ack_sum;    // 34  peer sum the sender has adopted, LE
    uint16_t body_len;   // 36  bytes following this header, LE
} mpg_hdr_t;

// peer_nonce is derivable (session ^ nonce), and carried anyway: it makes a
// mis-latched pairing visible in a hexdump instead of silent.
typedef struct __attribute__((packed)) {
    uint32_t nonce;       //  0  fresh per entry to Seeking; never 0
    uint32_t peer_nonce;  //  4  the nonce folded into `session`; 0 if none yet
    uint8_t  games;       //  8  bitmask of games this badge can play
    uint8_t  rsv[3];      //  9  zero
} mpg_hello_t;

// A whole chess position. Byte-identical to what chess_pack() writes, which
// the static_assert below and a host round-trip test both pin down.
//
// Square s lives in squares[s >> 1], EVEN s in the LOW nibble. Both ends must
// agree on that, so go through mpg_pos_get()/mpg_pos_set() rather than
// open-coding the shift — the same discipline as bbx_bitmap_test/set.
//
// Nibble values are ChessPiece: 0 empty, 1..6 white P N B R Q K, 9..14 black.
// 7, 8 and 15 are not pieces and MUST be rejected on receive; chess_unpack()
// does that.
typedef struct __attribute__((packed)) {
    uint8_t  squares[32];  //  0
    uint8_t  stm;          // 32  0 = white to move
    uint8_t  castling;     // 33  ChessCastle bits
    uint8_t  ep_file;      // 34  0..7, or CH_NO_EP; the rank is implied by stm
    uint8_t  halfmove;     // 35  plies since a pawn move or capture
    uint16_t fullmove;     // 36  LE, starts at 1
} mpg_pos_t;

// last_move is a ChessMove rather than a from/to byte pair specifically so
// there is ONE move representation across the engine, the API and the wire,
// with nowhere for a from/to swap to hide. result is a ChessResult.
typedef struct __attribute__((packed)) {
    mpg_pos_t pos;         //  0  the position AFTER last_move
    uint16_t  last_move;   // 38  CH_MOVE_NONE if no move has been played
    uint8_t   result;      // 40  ChessResult — the mover is authoritative
    uint8_t   rep_count;   // 41  sender's repetition count for pos
    uint8_t   in_check;    // 42  0/1. Pure bug-catcher: when `sum` matches the
                           //     positions are identical, so this must agree.
    uint8_t   rsv;         // 43  zero
} mpg_chess_body_t;

#define MPG_CHESS_FRAME (MPG_HDR_SIZE + (int)sizeof(mpg_chess_body_t))   // 82

static_assert(sizeof(mpg_hdr_t)        == MPG_HDR_SIZE, "MPG1 header must be 38 bytes");
static_assert(sizeof(mpg_hello_t)      == 12, "mpg_hello_t must be 12 bytes");
static_assert(sizeof(mpg_pos_t)        == 38, "mpg_pos_t must be 38 bytes");
static_assert(sizeof(mpg_chess_body_t) == 44, "mpg_chess_body_t must be 44 bytes");

// The packer and the wire struct are two descriptions of one layout. Tying
// them together here means adding a field to one without the other fails to
// compile rather than silently shifting every square by a byte.
static_assert(sizeof(mpg_pos_t) == CH_PACKED_SIZE,
              "mpg_pos_t and chess_pack() have drifted apart");

// Alignment, so the memcpy-straight-off-the-wire assumption holds.
static_assert(offsetof(mpg_hdr_t, session) % 4 == 0, "session must be 4-aligned");
static_assert(offsetof(mpg_hdr_t, seq)      % 2 == 0, "seq must be 2-aligned");
static_assert(offsetof(mpg_hdr_t, sum)      % 2 == 0, "sum must be 2-aligned");
static_assert(offsetof(mpg_hdr_t, ack_seq)  % 2 == 0, "ack_seq must be 2-aligned");
static_assert(offsetof(mpg_hdr_t, ack_sum)  % 2 == 0, "ack_sum must be 2-aligned");
static_assert(offsetof(mpg_hdr_t, body_len) % 2 == 0, "body_len must be 2-aligned");
static_assert(offsetof(mpg_pos_t, fullmove) % 2 == 0, "fullmove must be 2-aligned");
static_assert(offsetof(mpg_chess_body_t, last_move) % 2 == 0,
              "last_move must be 2-aligned");

// Every body must fit one frame alongside the header...
static_assert(sizeof(mpg_hello_t)      <= MPG_MAX_BODY, "HELLO body too large");
static_assert(sizeof(mpg_chess_body_t) <= MPG_MAX_BODY, "chess body too large");
// ...and the largest frame must survive BOTH HCI directions: transmissible
// while advertising, and deliverable in one report at the peer.
static_assert(MPG_CHESS_FRAME + MPG_AD_OVERHEAD <= MPG_HCI_MAX_ADV_DATA,
              "frame + AD wrapper exceeds the single-HCI-command TX limit");
static_assert(MPG_CHESS_FRAME + MPG_AD_OVERHEAD <= MPG_HCI_MAX_ADV_RPT,
              "frame + AD wrapper would arrive fragmented and be dropped");
static_assert(MPG_CHESS_FRAME <= MPG_MAX_FRAME, "frame exceeds hal_radio's cap");

static inline bool mpg_magic_ok(const mpg_hdr_t *h)
{
    return h->magic[0] == MPG_MAGIC_0 && h->magic[1] == MPG_MAGIC_1
        && h->magic[2] == MPG_MAGIC_2 && h->magic[3] == MPG_MAGIC_3;
}

static inline void mpg_set_magic(mpg_hdr_t *h)
{
    h->magic[0] = MPG_MAGIC_0;
    h->magic[1] = MPG_MAGIC_1;
    h->magic[2] = MPG_MAGIC_2;
    h->magic[3] = MPG_MAGIC_3;
}

// Nibble accessors. Nothing outside chess_pack()/chess_unpack() should be
// open-coding this shift.
static inline uint8_t mpg_pos_get(const mpg_pos_t *p, uint8_t sq)
{
    return (sq & 1) ? (uint8_t)(p->squares[sq >> 1] >> 4)
                    : (uint8_t)(p->squares[sq >> 1] & 0x0F);
}

static inline void mpg_pos_set(mpg_pos_t *p, uint8_t sq, uint8_t piece)
{
    uint8_t *b = &p->squares[sq >> 1];
    if (sq & 1) *b = (uint8_t)((*b & 0x0F) | (uint8_t)(piece << 4));
    else        *b = (uint8_t)((*b & 0xF0) | (uint8_t)(piece & 0x0F));
}

// CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final xor).
// Check value for "123456789" is 0x29B1.
//
// Deliberately not esp_rom_crc32_le(), which bbx uses: this header has to
// compile on a host so the round-trip tests in tools/chess-perft can run
// without an IDF toolchain. 16 bits is ample — the BLE link layer already
// carries its own 24-bit CRC, so this exists to catch a truncated or
// mis-parsed body, not line noise.
static inline uint16_t mpg_crc16(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}
