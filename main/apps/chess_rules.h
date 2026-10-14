#pragma once

#include <cstddef>
#include <cstdint>

// Full legal chess: movement, castling, en passant, promotion, check,
// checkmate, stalemate, the 50-move rule, threefold repetition and the
// insufficient-material dead positions.
//
// Deliberately pure C++17. No LVGL, no ESP-IDF, no allocation, no globals that
// change, and no recursion outside chess_perft() — which is test-only and
// slices a caller-supplied buffer so the recursion carries a pointer rather
// than a 436-byte array. That keeps this file compilable by plain g++ on a
// host, which is the whole point: the rules are verified against the standard
// perft suite in tools/chess-perft before any of it reaches a badge, without
// paying for a multi-minute IDF build to find a move-generation bug.
//
// The 7168-byte LVGL port task stack is the binding constraint on the badge
// (esp_lvgl_port.h:68, unmodified by hal_display.cpp). Two functions here
// place a ChessMove[218] — 436 bytes — on the stack; they are marked below.
// Everything else is frame-sized. Callers on a hot path should hold their own
// move array and use chess_gen_legal()/chess_moves_from().
//
// SQUARES ARE 0..63 EVERYWHERE IN THIS API. a1=0, h1=7, a8=56, h8=63, so
// rank = sq >> 3 and file = sq & 7. ChessPos::board is internally a 0x88
// mailbox and is the one place the other numbering exists; never index it
// directly, use chess_piece_at().

// Piece encoding. Chosen so colour is `p >> 3` and type is `p & 7`, and so the
// values pack directly into the wire format's nibbles with no mapping table:
// 0 is empty, 1..6 white, 9..14 black, and 7/8/15 are invalid and must be
// rejected on receive.
enum ChessPiece : uint8_t {
    CH_EMPTY = 0,
    CH_WP = 1, CH_WN = 2, CH_WB = 3, CH_WR = 4, CH_WQ = 5, CH_WK = 6,
    CH_BP = 9, CH_BN = 10, CH_BB = 11, CH_BR = 12, CH_BQ = 13, CH_BK = 14,
};

enum ChessColor : uint8_t { CH_WHITE = 0, CH_BLACK = 1 };

// Piece types, i.e. `piece & 7`. Also the values a move's promotion field takes.
enum ChessType : uint8_t {
    CH_NONE = 0, CH_PAWN = 1, CH_KNIGHT = 2, CH_BISHOP = 3,
    CH_ROOK = 4, CH_QUEEN = 5, CH_KING = 6,
};

enum ChessCastle : uint8_t {
    CH_WK_SIDE = 1, CH_WQ_SIDE = 2, CH_BK_SIDE = 4, CH_BQ_SIDE = 8,
};

enum class ChessResult : uint8_t {
    Playing = 0,
    WhiteMates, BlackMates,
    Stalemate, DrawFifty, DrawRepetition, DrawMaterial, DrawAgreed,
    WhiteResigns, BlackResigns,
    Aborted,
};

// One move representation for the engine, the public API and the wire, so
// there is nowhere for a from/to swap to hide. Promotion holds a ChessType
// (CH_KNIGHT..CH_QUEEN), or CH_NONE.
typedef uint16_t ChessMove;

// from == to is never legal, so the all-ones word is a safe sentinel.
static constexpr ChessMove CH_MOVE_NONE = 0xFFFF;

// The most legal moves any chess position is known to admit.
static constexpr int CH_MAX_MOVES = 218;

// Repetition history. Only positions since the last irreversible move can
// repeat, so this is bounded by the halfmove clock (100 plies before the
// 50-move rule ends the game), not by game length. 128 gives slack for a game
// nobody claims a draw in; past that the oldest pair is dropped.
static constexpr int CH_HIST_MAX = 128;

// Bytes chess_pack() writes. Phase 1 static_asserts sizeof(mpg_pos_t) against
// this so the wire struct and the packer can never drift apart.
static constexpr int CH_PACKED_SIZE = 38;

// 0xFF means "no en passant target". Otherwise 0..7 is the target's file; the
// rank is implied by the side to move (rank 6 when White is to move, because
// Black must have been the one who just double-pushed).
static constexpr uint8_t CH_NO_EP = 0xFF;

// `board` is a 0x88 mailbox: index is rank*16 + file, and a square is off the
// board iff (index & 0x88) != 0, which is one AND per ray step. `king` holds
// each side's king square in that same 0x88 numbering, maintained by
// chess_make()/chess_unmake() so check tests never have to search for it.
struct ChessPos {
    uint8_t  board[128];
    uint8_t  stm;       // side to move, CH_WHITE / CH_BLACK
    uint8_t  castling;  // ChessCastle bits
    uint8_t  ep_file;   // 0..7, or CH_NO_EP
    uint8_t  halfmove;  // plies since a pawn move or capture
    uint16_t fullmove;  // starts at 1, increments after Black moves
    uint8_t  king[2];   // 0x88 square, indexed by colour
};

// Everything chess_make() destroys that the move itself does not imply.
// `cap_sq` differs from the move's `to` only for en passant, which is exactly
// why unmake can restore an ep capture without special-casing it.
struct ChessUndo {
    uint8_t captured;
    uint8_t cap_sq;     // 0x88
    uint8_t castling;
    uint8_t ep_file;
    uint8_t halfmove;
    uint8_t king_from;  // 0x88, the mover's king before the move
};

// A position plus the history the repetition rule needs. `hist` holds Zobrist
// keys of every position since the last irreversible move, including the
// current one, so hist_len is never 0 for an initialised game.
struct ChessGame {
    ChessPos    pos;
    uint64_t    hist[CH_HIST_MAX];
    uint8_t     hist_len;
    ChessResult result;
};

// --- move field access ------------------------------------------------------

static inline uint8_t ch_move_from(ChessMove m)  { return (uint8_t)(m & 63); }
static inline uint8_t ch_move_to(ChessMove m)    { return (uint8_t)((m >> 6) & 63); }
static inline uint8_t ch_move_promo(ChessMove m) { return (uint8_t)((m >> 12) & 15); }

static inline ChessMove ch_move(uint8_t from, uint8_t to, uint8_t promo = CH_NONE)
{
    return (ChessMove)(from | (to << 6) | (promo << 12));
}

// Colour and type of a piece. Both are meaningless for CH_EMPTY, so check
// `piece != CH_EMPTY` first.
static inline uint8_t ch_color(uint8_t piece) { return (uint8_t)(piece >> 3); }
static inline uint8_t ch_type(uint8_t piece)  { return (uint8_t)(piece & 7); }

// --- position queries -------------------------------------------------------

void    chess_start_pos(ChessPos *p);
uint8_t chess_piece_at(const ChessPos *p, uint8_t sq64);

// Rejects anything we would refuse to adopt from a peer: a bad piece nibble,
// not exactly one king a side, a pawn on rank 1 or 8, castling rights that do
// not match where the kings and rooks actually are, an impossible ep target,
// or — the one that matters most — the side NOT to move standing in check,
// which no legal move could have produced.
bool chess_pos_valid(const ChessPos *p);

bool chess_square_attacked(const ChessPos *p, uint8_t sq64, uint8_t by_side);
bool chess_in_check(const ChessPos *p, uint8_t side);

// True if a pawn move from->to lands on the last rank, i.e. the caller must
// pick a promotion piece before the move is complete.
bool chess_needs_promotion(const ChessPos *p, uint8_t from64, uint8_t to64);

// --- move generation --------------------------------------------------------
//
// These take a mutable position because legality filtering runs make / king
// attacked / unmake. The position is restored exactly; treat it as const.

// Writes every legal move into `out` and returns the count. `cap` should be
// CH_MAX_MOVES; a smaller buffer truncates rather than overruns.
int  chess_gen_legal(ChessPos *p, ChessMove *out, int cap);

// Just the legal moves for the piece on `from64`. What the UI calls when a
// player selects a piece.
int  chess_moves_from(ChessPos *p, uint8_t from64, ChessMove *out, int cap);

bool chess_is_legal(ChessPos *p, ChessMove m);

// chess_make() returns false and changes nothing if the move does not start on
// a piece of the side to move. It does NOT check legality — that is
// chess_is_legal()'s job, and the filter inside chess_gen_legal() depends on
// being able to make a pseudo-legal move and look at the result.
bool chess_make(ChessPos *p, ChessMove m, ChessUndo *u);
void chess_unmake(ChessPos *p, ChessMove m, const ChessUndo *u);

// --- game (position + history + adjudication) -------------------------------

void chess_game_reset(ChessGame *g);

// Validates, plays, appends to history and adjudicates. False (and no change)
// if the move is not legal or the game is already over.
// STACK: places a ChessMove[218] (436 bytes).
bool chess_game_play(ChessGame *g, ChessMove m);

// Take a position from a peer. `contiguous` means it is the direct successor
// of what we already had, so repetition history carries forward; otherwise the
// history is reset to just this position, because we cannot know what we
// missed. False if the position fails chess_pos_valid().
// STACK: places a ChessMove[218] (436 bytes).
bool chess_game_adopt(ChessGame *g, const ChessPos *p, bool contiguous);

uint64_t chess_hash(const ChessPos *p);

// How many times the current position has occurred. 1 is the first occurrence,
// so 3 is a claimable threefold repetition.
int chess_repetitions(const ChessGame *g);

// STACK: places a ChessMove[218] (436 bytes).
ChessResult chess_adjudicate(ChessGame *g);

bool chess_result_is_over(ChessResult r);
const char *chess_result_name(ChessResult r);

// --- serialisation ----------------------------------------------------------
//
// The packed form is the wire body (mpg_pos_t): 32 bytes of nibble-packed
// squares, then stm, castling, ep_file, halfmove, and a little-endian
// fullmove. Square s lives in byte s>>1, EVEN s in the low nibble. Both ends
// must agree on that, so nothing outside this file open-codes the shift.

void chess_pack(const ChessPos *p, uint8_t out[CH_PACKED_SIZE]);

// False on an invalid piece nibble (7, 8, 15), a bad ep_file, or anything
// chess_pos_valid() rejects. A peer frame that fails this is dropped, not
// adopted.
bool chess_unpack(ChessPos *p, const uint8_t in[CH_PACKED_SIZE]);

// FEN earns its place by making every rules bug reproducible from one console
// line, and by letting the host harness drive the standard perft positions.
size_t chess_to_fen(const ChessPos *p, char *out, size_t out_sz);
bool   chess_from_fen(ChessPos *p, const char *fen);

// "e2e4", "e7e8q". `out` needs 6 bytes.
void chess_move_to_uci(ChessMove m, char out[6]);
bool chess_move_from_uci(ChessPos *p, const char *s, ChessMove *out);

// --- test only --------------------------------------------------------------

// Recursive. `scratch` must hold CH_MAX_MOVES * depth entries; the recursion
// slices it rather than allocating a move array per frame, which is what keeps
// a depth-4 run inside ~150 bytes of stack per level on the badge.
uint64_t chess_perft(ChessPos *p, int depth, ChessMove *scratch);
