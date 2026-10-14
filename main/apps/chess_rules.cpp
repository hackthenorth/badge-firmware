#include "chess_rules.h"

#include <cstdio>
#include <cstring>

// 0x88 mailbox. Index is rank*16 + file, so a square is off the board iff
// (index & 0x88) != 0 — one AND per ray step, which is the whole reason for
// the representation. Bitboards would be the wrong tool here: the C3 is a
// 32-bit RV32IMC with no popcount and no 64-bit registers, magic tables run to
// 100+ KB of flash, and the only thing they buy is search throughput. There is
// no AI, so move generation runs a handful of times per button press.
//
// The cost, stated honestly, is that half the array is wasted (128 bytes,
// irrelevant) and that squares need converting at the API boundary. That
// conversion lives in exactly the two inline functions below and is applied
// only where a move crosses in or out, because moves are 0..63 everywhere.

namespace {

inline uint8_t to88(uint8_t sq64)  { return (uint8_t)(sq64 + (sq64 & ~7)); }
inline uint8_t to64(uint8_t sq88)  { return (uint8_t)((sq88 + (sq88 & 7)) >> 1); }
inline bool    on_board(int sq88)  { return (sq88 & ~0x77) == 0; }
inline int     rank88(uint8_t sq)  { return sq >> 4; }
inline int     file88(uint8_t sq)  { return sq & 7; }
inline int     sq_color88(uint8_t sq) { return (rank88(sq) + file88(sq)) & 1; }

// on_board() as written also rejects negatives once the int is sign-extended,
// which matters because ray walking produces them: -1 has every high bit set.

constexpr int kKnightDir[8] = { -33, -31, -18, -14, 14, 18, 31, 33 };
constexpr int kBishopDir[4] = { -17, -15, 15, 17 };
constexpr int kRookDir[4]   = { -16, -1, 1, 16 };
constexpr int kKingDir[8]   = { -17, -16, -15, -1, 1, 15, 16, 17 };

// Home squares, 0x88.
constexpr uint8_t A1 = 0x00, B1 = 0x01, C1 = 0x02, D1 = 0x03;
constexpr uint8_t E1 = 0x04, F1 = 0x05, G1 = 0x06, H1 = 0x07;
constexpr uint8_t A8 = 0x70, B8 = 0x71, C8 = 0x72, D8 = 0x73;
constexpr uint8_t E8 = 0x74, F8 = 0x75, G8 = 0x76, H8 = 0x77;

// Castling rights surviving a move that touches a square. ANDing the mask for
// both `from` and `to` handles every case in one line: the king moving, a rook
// moving, and — the one that is easy to forget — a rook being *captured* on
// its home square.
struct CastleMask {
    uint8_t m[128];
    constexpr CastleMask() : m{} {
        for (int i = 0; i < 128; ++i) m[i] = 0xFF;
        m[E1] = (uint8_t)~(CH_WK_SIDE | CH_WQ_SIDE);
        m[H1] = (uint8_t)~CH_WK_SIDE;
        m[A1] = (uint8_t)~CH_WQ_SIDE;
        m[E8] = (uint8_t)~(CH_BK_SIDE | CH_BQ_SIDE);
        m[H8] = (uint8_t)~CH_BK_SIDE;
        m[A8] = (uint8_t)~CH_BQ_SIDE;
    }
};
constexpr CastleMask kCastle{};

// --- Zobrist ----------------------------------------------------------------
//
// Built at compile time so the table lands in flash and, more importantly, so
// both badges compute identical keys from identical source. Repetition counts
// are compared across the link (mpg_chess_body_t::rep_count), so a divergent
// table would show up as two badges disagreeing about a draw.
//
// 64-bit, not 32: at 300 plies a 32-bit key collides about once per 100,000
// games, which over an event's worth of play is a draw claimed out of nowhere.

constexpr uint64_t splitmix64(uint64_t &s)
{
    s += 0x9E3779B97F4A7C15ull;
    uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

struct Zobrist {
    uint64_t piece[16][64];
    uint64_t castling[16];
    uint64_t ep[8];
    uint64_t side;
    constexpr Zobrist() : piece{}, castling{}, ep{}, side{}
    {
        uint64_t s = 0x246C7A1B5E3D9F02ull;
        for (int p = 0; p < 16; ++p)
            for (int q = 0; q < 64; ++q) piece[p][q] = splitmix64(s);
        for (int i = 0; i < 16; ++i) castling[i] = splitmix64(s);
        for (int i = 0; i < 8; ++i) ep[i] = splitmix64(s);
        side = splitmix64(s);
    }
};
constexpr Zobrist kZob{};

// En passant target as a 0x88 square, or 0xFF. The rank is implied by the side
// to move: White to move means Black just double-pushed, so the target is on
// rank 6. 0xFF is safely off-board, so it never matches a real destination.
inline uint8_t ep_sq88(const ChessPos *p)
{
    if (p->ep_file > 7) return 0xFF;
    return (uint8_t)((p->stm == CH_WHITE ? 0x50 : 0x20) + p->ep_file);
}

bool attacked88(const ChessPos *p, uint8_t sq, uint8_t by)
{
    // Pawns. A white pawn on x attacks x+15 and x+17, so a white pawn
    // attacking `sq` sits at sq-15 or sq-17. Getting this backwards is the
    // classic bug and it only shows up in check detection, never in movement.
    if (by == CH_WHITE) {
        if (on_board(sq - 15) && p->board[sq - 15] == CH_WP) return true;
        if (on_board(sq - 17) && p->board[sq - 17] == CH_WP) return true;
    } else {
        if (on_board(sq + 15) && p->board[sq + 15] == CH_BP) return true;
        if (on_board(sq + 17) && p->board[sq + 17] == CH_BP) return true;
    }

    const uint8_t knight = (uint8_t)(CH_KNIGHT | (by << 3));
    for (int d : kKnightDir) {
        int t = sq + d;
        if (on_board(t) && p->board[t] == knight) return true;
    }

    const uint8_t king = (uint8_t)(CH_KING | (by << 3));
    for (int d : kKingDir) {
        int t = sq + d;
        if (on_board(t) && p->board[t] == king) return true;
    }

    const uint8_t bishop = (uint8_t)(CH_BISHOP | (by << 3));
    const uint8_t queen  = (uint8_t)(CH_QUEEN  | (by << 3));
    for (int d : kBishopDir) {
        for (int t = sq + d; on_board(t); t += d) {
            uint8_t pc = p->board[t];
            if (pc == CH_EMPTY) continue;
            if (pc == bishop || pc == queen) return true;
            break;
        }
    }

    const uint8_t rook = (uint8_t)(CH_ROOK | (by << 3));
    for (int d : kRookDir) {
        for (int t = sq + d; on_board(t); t += d) {
            uint8_t pc = p->board[t];
            if (pc == CH_EMPTY) continue;
            if (pc == rook || pc == queen) return true;
            break;
        }
    }

    return false;
}

struct Emit {
    ChessMove *out;
    int cap;
    int n = 0;
    inline void add(uint8_t from88, uint8_t to88_, uint8_t promo = CH_NONE)
    {
        if (n < cap) out[n++] = ch_move(to64(from88), to64(to88_), promo);
    }
    inline void add_pawn(uint8_t from88, uint8_t to88_, int promo_rank)
    {
        if (rank88(to88_) == promo_rank) {
            add(from88, to88_, CH_QUEEN);
            add(from88, to88_, CH_ROOK);
            add(from88, to88_, CH_BISHOP);
            add(from88, to88_, CH_KNIGHT);
        } else {
            add(from88, to88_);
        }
    }
};

void gen_piece(const ChessPos *p, uint8_t sq, Emit &e)
{
    const uint8_t pc   = p->board[sq];
    const uint8_t side = ch_color(pc);
    const uint8_t type = ch_type(pc);

    if (type == CH_PAWN) {
        const int fwd        = (side == CH_WHITE) ? 16 : -16;
        const int start_rank = (side == CH_WHITE) ? 1 : 6;
        const int promo_rank = (side == CH_WHITE) ? 7 : 0;
        const uint8_t ep     = ep_sq88(p);

        int one = sq + fwd;
        if (on_board(one) && p->board[one] == CH_EMPTY) {
            e.add_pawn(sq, (uint8_t)one, promo_rank);
            int two = one + fwd;
            if (rank88(sq) == start_rank && on_board(two) && p->board[two] == CH_EMPTY)
                e.add(sq, (uint8_t)two);
        }
        const int cap_dir[2] = { fwd - 1, fwd + 1 };
        for (int d : cap_dir) {
            int t = sq + d;
            if (!on_board(t)) continue;
            uint8_t target = p->board[t];
            if (target != CH_EMPTY) {
                if (ch_color(target) != side) e.add_pawn(sq, (uint8_t)t, promo_rank);
            } else if ((uint8_t)t == ep) {
                e.add(sq, (uint8_t)t);
            }
        }
        return;
    }

    if (type == CH_KNIGHT || type == CH_KING) {
        const int *dirs = (type == CH_KNIGHT) ? kKnightDir : kKingDir;
        for (int i = 0; i < 8; ++i) {
            int t = sq + dirs[i];
            if (!on_board(t)) continue;
            uint8_t target = p->board[t];
            if (target == CH_EMPTY || ch_color(target) != side) e.add(sq, (uint8_t)t);
        }
        return;
    }

    // Sliders. Bishop and rook each walk their own four rays; a queen walks
    // both sets, which is why this is two loops rather than a direction table
    // indexed by piece.
    if (type == CH_BISHOP || type == CH_QUEEN) {
        for (int d : kBishopDir) {
            for (int t = sq + d; on_board(t); t += d) {
                uint8_t target = p->board[t];
                if (target == CH_EMPTY) { e.add(sq, (uint8_t)t); continue; }
                if (ch_color(target) != side) e.add(sq, (uint8_t)t);
                break;
            }
        }
    }
    if (type == CH_ROOK || type == CH_QUEEN) {
        for (int d : kRookDir) {
            for (int t = sq + d; on_board(t); t += d) {
                uint8_t target = p->board[t];
                if (target == CH_EMPTY) { e.add(sq, (uint8_t)t); continue; }
                if (ch_color(target) != side) e.add(sq, (uint8_t)t);
                break;
            }
        }
    }
}

// Castling is filtered here rather than in the legality pass, because "the
// king may not pass through an attacked square" is not a "the king ends up in
// check" test and the generic filter cannot see it. The destination square is
// checked here too — redundant with the filter, but keeping all three
// together is what makes the rule readable.
void gen_castles(const ChessPos *p, Emit &e)
{
    const uint8_t side = p->stm;
    const uint8_t opp  = (uint8_t)(side ^ 1);

    if (side == CH_WHITE) {
        if (p->board[E1] != CH_WK) return;
        if ((p->castling & CH_WK_SIDE) && p->board[H1] == CH_WR
            && p->board[F1] == CH_EMPTY && p->board[G1] == CH_EMPTY
            && !attacked88(p, E1, opp) && !attacked88(p, F1, opp)
            && !attacked88(p, G1, opp)) {
            e.add(E1, G1);
        }
        // b1 must be empty but need not be safe: the king never visits it.
        if ((p->castling & CH_WQ_SIDE) && p->board[A1] == CH_WR
            && p->board[D1] == CH_EMPTY && p->board[C1] == CH_EMPTY
            && p->board[B1] == CH_EMPTY
            && !attacked88(p, E1, opp) && !attacked88(p, D1, opp)
            && !attacked88(p, C1, opp)) {
            e.add(E1, C1);
        }
    } else {
        if (p->board[E8] != CH_BK) return;
        if ((p->castling & CH_BK_SIDE) && p->board[H8] == CH_BR
            && p->board[F8] == CH_EMPTY && p->board[G8] == CH_EMPTY
            && !attacked88(p, E8, opp) && !attacked88(p, F8, opp)
            && !attacked88(p, G8, opp)) {
            e.add(E8, G8);
        }
        if ((p->castling & CH_BQ_SIDE) && p->board[A8] == CH_BR
            && p->board[D8] == CH_EMPTY && p->board[C8] == CH_EMPTY
            && p->board[B8] == CH_EMPTY
            && !attacked88(p, E8, opp) && !attacked88(p, D8, opp)
            && !attacked88(p, C8, opp)) {
            e.add(E8, C8);
        }
    }
}

// `only88` restricts generation to one origin square (what the UI needs when a
// piece is selected), or -1 for the whole position.
int gen_pseudo(const ChessPos *p, ChessMove *out, int cap, int only88)
{
    Emit e{ out, cap };
    if (only88 >= 0) {
        uint8_t sq = (uint8_t)only88;
        uint8_t pc = p->board[sq];
        if (pc != CH_EMPTY && ch_color(pc) == p->stm) gen_piece(p, sq, e);
        if (ch_type(pc) == CH_KING && pc != CH_EMPTY && ch_color(pc) == p->stm)
            gen_castles(p, e);
        return e.n;
    }
    for (uint8_t sq = 0; sq <= 0x77; ++sq) {
        if (sq & 0x88) continue;
        uint8_t pc = p->board[sq];
        if (pc != CH_EMPTY && ch_color(pc) == p->stm) gen_piece(p, sq, e);
    }
    gen_castles(p, e);
    return e.n;
}

int filter_legal(ChessPos *p, ChessMove *out, int n)
{
    int k = 0;
    for (int i = 0; i < n; ++i) {
        ChessUndo u;
        uint8_t side = p->stm;
        ChessMove m = out[i];
        if (!chess_make(p, m, &u)) continue;
        if (!attacked88(p, p->king[side], (uint8_t)(side ^ 1))) out[k++] = m;
        chess_unmake(p, m, &u);
    }
    return k;
}

bool insufficient_material(const ChessPos *p)
{
    int minors[2] = { 0, 0 };
    int bishops[2] = { 0, 0 };
    int bcolor[2] = { -1, -1 };

    for (uint8_t sq = 0; sq <= 0x77; ++sq) {
        if (sq & 0x88) continue;
        uint8_t pc = p->board[sq];
        if (pc == CH_EMPTY) continue;
        uint8_t t = ch_type(pc);
        if (t == CH_KING) continue;
        // A pawn can promote, a rook or queen mates on its own. Any of them
        // and the position is alive.
        if (t == CH_PAWN || t == CH_ROOK || t == CH_QUEEN) return false;
        uint8_t c = ch_color(pc);
        minors[c]++;
        if (t == CH_BISHOP) { bishops[c]++; bcolor[c] = sq_color88(sq); }
    }

    if (minors[0] == 0 && minors[1] == 0) return true;          // K vs K
    if (minors[0] + minors[1] == 1) return true;                // K+minor vs K
    // K+B vs K+B on the same square colour. Note KNN vs K is deliberately NOT
    // here: mate is impossible to force but possible to reach with help, so
    // FIDE does not call it a dead position.
    if (minors[0] == 1 && minors[1] == 1 && bishops[0] == 1 && bishops[1] == 1
        && bcolor[0] == bcolor[1]) return true;
    return false;
}

void hist_push(ChessGame *g, uint64_t key)
{
    if (g->hist_len >= CH_HIST_MAX) {
        // Only reachable if both players decline a 50-move draw. Drop the
        // oldest PAIR, so the parity the step-2 scan in chess_repetitions()
        // relies on is preserved.
        memmove(g->hist, g->hist + 2, (CH_HIST_MAX - 2) * sizeof(uint64_t));
        g->hist_len = CH_HIST_MAX - 2;
    }
    g->hist[g->hist_len++] = key;
}

const char kPieceChar[16] = {
    '.', 'P', 'N', 'B', 'R', 'Q', 'K', '?',
    '?', 'p', 'n', 'b', 'r', 'q', 'k', '?',
};

uint8_t piece_from_char(char c)
{
    switch (c) {
        case 'P': return CH_WP;  case 'N': return CH_WN;  case 'B': return CH_WB;
        case 'R': return CH_WR;  case 'Q': return CH_WQ;  case 'K': return CH_WK;
        case 'p': return CH_BP;  case 'n': return CH_BN;  case 'b': return CH_BB;
        case 'r': return CH_BR;  case 'q': return CH_BQ;  case 'k': return CH_BK;
        default:  return CH_EMPTY;
    }
}

}  // namespace

// --- position ---------------------------------------------------------------

void chess_start_pos(ChessPos *p)
{
    memset(p, 0, sizeof(*p));
    static const uint8_t back[8] = { CH_WR, CH_WN, CH_WB, CH_WQ,
                                     CH_WK, CH_WB, CH_WN, CH_WR };
    for (int f = 0; f < 8; ++f) {
        p->board[0x00 + f] = back[f];
        p->board[0x10 + f] = CH_WP;
        p->board[0x60 + f] = CH_BP;
        p->board[0x70 + f] = (uint8_t)(back[f] | 8);
    }
    p->stm      = CH_WHITE;
    p->castling = CH_WK_SIDE | CH_WQ_SIDE | CH_BK_SIDE | CH_BQ_SIDE;
    p->ep_file  = CH_NO_EP;
    p->halfmove = 0;
    p->fullmove = 1;
    p->king[CH_WHITE] = E1;
    p->king[CH_BLACK] = E8;
}

uint8_t chess_piece_at(const ChessPos *p, uint8_t sq64)
{
    if (sq64 > 63) return CH_EMPTY;
    return p->board[to88(sq64)];
}

bool chess_pos_valid(const ChessPos *p)
{
    if (p->stm > 1) return false;
    if (p->castling > 15) return false;
    if (p->ep_file != CH_NO_EP && p->ep_file > 7) return false;

    int kings[2] = { 0, 0 };
    int count[2] = { 0, 0 };
    uint8_t ksq[2] = { 0xFF, 0xFF };

    for (uint8_t sq = 0; sq <= 0x77; ++sq) {
        if (sq & 0x88) continue;
        uint8_t pc = p->board[sq];
        if (pc == CH_EMPTY) continue;
        uint8_t t = ch_type(pc);
        // Catches the invalid wire nibbles 7, 8 and 15 in one test: 8 has
        // type 0 and 7/15 have type 7.
        if (t == CH_NONE || t > CH_KING) return false;
        uint8_t c = ch_color(pc);
        if (t == CH_PAWN && (rank88(sq) == 0 || rank88(sq) == 7)) return false;
        if (t == CH_KING) { kings[c]++; ksq[c] = sq; }
        if (++count[c] > 16) return false;
    }
    if (kings[0] != 1 || kings[1] != 1) return false;

    // Internal consistency: every construction path in this file maintains
    // king[], and chess_in_check() trusts it, so a mismatch is a real bug
    // rather than merely an odd position.
    if (p->king[0] != ksq[0] || p->king[1] != ksq[1]) return false;

    if ((p->castling & CH_WK_SIDE) && (p->board[E1] != CH_WK || p->board[H1] != CH_WR))
        return false;
    if ((p->castling & CH_WQ_SIDE) && (p->board[E1] != CH_WK || p->board[A1] != CH_WR))
        return false;
    if ((p->castling & CH_BK_SIDE) && (p->board[E8] != CH_BK || p->board[H8] != CH_BR))
        return false;
    if ((p->castling & CH_BQ_SIDE) && (p->board[E8] != CH_BK || p->board[A8] != CH_BR))
        return false;

    if (p->ep_file != CH_NO_EP) {
        const int tr  = (p->stm == CH_WHITE) ? 5 : 2;   // the target square
        const int pr  = (p->stm == CH_WHITE) ? 4 : 3;   // the pawn that moved
        const int orr = (p->stm == CH_WHITE) ? 6 : 1;   // where it came from
        const uint8_t want = (p->stm == CH_WHITE) ? CH_BP : CH_WP;
        const uint8_t f = p->ep_file;
        if (p->board[tr * 16 + f] != CH_EMPTY) return false;
        if (p->board[orr * 16 + f] != CH_EMPTY) return false;
        if (p->board[pr * 16 + f] != want) return false;
    }

    // The side that just moved cannot have left its own king en prise — no
    // legal move produces that, so such a frame is corrupt or hostile.
    if (attacked88(p, ksq[p->stm ^ 1], p->stm)) return false;

    return true;
}

bool chess_square_attacked(const ChessPos *p, uint8_t sq64, uint8_t by_side)
{
    if (sq64 > 63) return false;
    return attacked88(p, to88(sq64), (uint8_t)(by_side & 1));
}

bool chess_in_check(const ChessPos *p, uint8_t side)
{
    side &= 1;
    return attacked88(p, p->king[side], (uint8_t)(side ^ 1));
}

bool chess_needs_promotion(const ChessPos *p, uint8_t from64, uint8_t to64_)
{
    if (from64 > 63 || to64_ > 63) return false;
    uint8_t pc = p->board[to88(from64)];
    if (pc == CH_EMPTY || ch_type(pc) != CH_PAWN) return false;
    int r = to64_ >> 3;
    return ch_color(pc) == CH_WHITE ? (r == 7) : (r == 0);
}

// --- make / unmake ----------------------------------------------------------

bool chess_make(ChessPos *p, ChessMove m, ChessUndo *u)
{
    const uint8_t from  = to88(ch_move_from(m));
    const uint8_t to    = to88(ch_move_to(m));
    const uint8_t promo = ch_move_promo(m);

    const uint8_t piece = p->board[from];
    if (piece == CH_EMPTY || ch_color(piece) != p->stm) return false;

    const uint8_t side = p->stm;
    const uint8_t type = ch_type(piece);

    u->castling  = p->castling;
    u->ep_file   = p->ep_file;
    u->halfmove  = p->halfmove;
    u->king_from = p->king[side];
    u->captured  = p->board[to];
    u->cap_sq    = to;

    // En passant: a pawn changing file onto an empty square can only be one.
    // Recording cap_sq separately is what lets unmake restore it without
    // knowing anything about ep.
    if (type == CH_PAWN && u->captured == CH_EMPTY && file88(from) != file88(to)) {
        u->cap_sq   = (uint8_t)(side == CH_WHITE ? to - 16 : to + 16);
        u->captured = p->board[u->cap_sq];
        p->board[u->cap_sq] = CH_EMPTY;
    }

    p->board[from] = CH_EMPTY;
    p->board[to]   = promo ? (uint8_t)(promo | (side << 3)) : piece;

    if (type == CH_KING) {
        p->king[side] = to;
        int d = (int)to - (int)from;
        if (d == 2) {                       // O-O: rook h -> f
            p->board[from + 1] = p->board[from + 3];
            p->board[from + 3] = CH_EMPTY;
        } else if (d == -2) {               // O-O-O: rook a -> d
            p->board[from - 1] = p->board[from - 4];
            p->board[from - 4] = CH_EMPTY;
        }
    }

    p->ep_file = CH_NO_EP;
    if (type == CH_PAWN) {
        int d = (int)to - (int)from;
        if (d == 32 || d == -32) p->ep_file = (uint8_t)file88(from);
    }

    if (type == CH_PAWN || u->captured != CH_EMPTY) p->halfmove = 0;
    else if (p->halfmove < 255) p->halfmove++;

    p->castling &= (uint8_t)(kCastle.m[from] & kCastle.m[to]);
    if (side == CH_BLACK) p->fullmove++;
    p->stm = (uint8_t)(side ^ 1);
    return true;
}

void chess_unmake(ChessPos *p, ChessMove m, const ChessUndo *u)
{
    const uint8_t from  = to88(ch_move_from(m));
    const uint8_t to    = to88(ch_move_to(m));
    const uint8_t promo = ch_move_promo(m);

    const uint8_t side = (uint8_t)(p->stm ^ 1);
    p->stm = side;
    if (side == CH_BLACK) p->fullmove--;

    uint8_t piece = p->board[to];
    if (promo) piece = (uint8_t)(CH_PAWN | (side << 3));

    p->board[from] = piece;
    p->board[to]   = CH_EMPTY;
    // Order matters: clear `to` first, then restore. For a normal capture
    // cap_sq == to and the piece goes straight back; for en passant they
    // differ and both squares end up right.
    if (u->captured != CH_EMPTY) p->board[u->cap_sq] = u->captured;

    if (ch_type(piece) == CH_KING) {
        p->king[side] = u->king_from;
        int d = (int)to - (int)from;
        if (d == 2) {
            p->board[from + 3] = p->board[from + 1];
            p->board[from + 1] = CH_EMPTY;
        } else if (d == -2) {
            p->board[from - 4] = p->board[from - 1];
            p->board[from - 1] = CH_EMPTY;
        }
    }

    p->castling = u->castling;
    p->ep_file  = u->ep_file;
    p->halfmove = u->halfmove;
}

// --- generation -------------------------------------------------------------

int chess_gen_legal(ChessPos *p, ChessMove *out, int cap)
{
    int n = gen_pseudo(p, out, cap, -1);
    return filter_legal(p, out, n);
}

int chess_moves_from(ChessPos *p, uint8_t from64, ChessMove *out, int cap)
{
    if (from64 > 63) return 0;
    int n = gen_pseudo(p, out, cap, (int)to88(from64));
    return filter_legal(p, out, n);
}

bool chess_is_legal(ChessPos *p, ChessMove m)
{
    if (m == CH_MOVE_NONE) return false;
    uint8_t from = ch_move_from(m);
    // A single origin square yields at most 27 moves (a queen), or 4 for a
    // promoting pawn. 64 is generous and keeps this off the 436-byte path, so
    // the hot "is this button press legal?" call stays frame-sized.
    ChessMove buf[64];
    int n = chess_moves_from(p, from, buf, 64);
    for (int i = 0; i < n; ++i) if (buf[i] == m) return true;
    return false;
}

// --- game -------------------------------------------------------------------

void chess_game_reset(ChessGame *g)
{
    chess_start_pos(&g->pos);
    g->hist_len = 0;
    g->result   = ChessResult::Playing;
    hist_push(g, chess_hash(&g->pos));
}

uint64_t chess_hash(const ChessPos *p)
{
    uint64_t h = 0;
    for (int r = 0; r < 8; ++r) {
        for (int f = 0; f < 8; ++f) {
            uint8_t pc = p->board[r * 16 + f];
            if (pc != CH_EMPTY) h ^= kZob.piece[pc][r * 8 + f];
        }
    }
    if (p->stm == CH_BLACK) h ^= kZob.side;
    h ^= kZob.castling[p->castling & 15];
    // Folded in unconditionally rather than only when a capture is actually
    // available. That makes repetition detection very slightly conservative
    // (it can miss a claim) and never wrong, and an ep file only survives one
    // ply anyway.
    if (p->ep_file < 8) h ^= kZob.ep[p->ep_file];
    return h;
}

int chess_repetitions(const ChessGame *g)
{
    if (g->hist_len == 0) return 0;
    const uint64_t cur = g->hist[g->hist_len - 1];
    int n = 0;
    // Only same-side-to-move positions can repeat, so step by two.
    for (int i = (int)g->hist_len - 1; i >= 0; i -= 2)
        if (g->hist[i] == cur) n++;
    return n;
}

ChessResult chess_adjudicate(ChessGame *g)
{
    ChessMove moves[CH_MAX_MOVES];
    int n = chess_gen_legal(&g->pos, moves, CH_MAX_MOVES);
    if (n == 0) {
        if (chess_in_check(&g->pos, g->pos.stm))
            return g->pos.stm == CH_WHITE ? ChessResult::BlackMates
                                          : ChessResult::WhiteMates;
        return ChessResult::Stalemate;
    }
    // Checkmate outranks every draw rule, which is why it is tested first.
    if (insufficient_material(&g->pos)) return ChessResult::DrawMaterial;
    if (g->pos.halfmove >= 100) return ChessResult::DrawFifty;
    if (chess_repetitions(g) >= 3) return ChessResult::DrawRepetition;
    return ChessResult::Playing;
}

bool chess_game_play(ChessGame *g, ChessMove m)
{
    if (chess_result_is_over(g->result)) return false;
    if (!chess_is_legal(&g->pos, m)) return false;

    ChessUndo u;
    if (!chess_make(&g->pos, m, &u)) return false;

    // halfmove is 0 exactly when the move was a pawn move or a capture, and
    // no position before an irreversible move can ever recur.
    if (g->pos.halfmove == 0) g->hist_len = 0;
    hist_push(g, chess_hash(&g->pos));

    g->result = chess_adjudicate(g);
    return true;
}

bool chess_game_adopt(ChessGame *g, const ChessPos *p, bool contiguous)
{
    if (!chess_pos_valid(p)) return false;
    g->pos = *p;
    if (!contiguous || g->hist_len == 0 || p->halfmove == 0) g->hist_len = 0;
    hist_push(g, chess_hash(&g->pos));
    g->result = chess_adjudicate(g);
    return true;
}

bool chess_result_is_over(ChessResult r) { return r != ChessResult::Playing; }

const char *chess_result_name(ChessResult r)
{
    switch (r) {
        case ChessResult::Playing:        return "Playing";
        case ChessResult::WhiteMates:     return "WhiteMates";
        case ChessResult::BlackMates:     return "BlackMates";
        case ChessResult::Stalemate:      return "Stalemate";
        case ChessResult::DrawFifty:      return "DrawFifty";
        case ChessResult::DrawRepetition: return "DrawRepetition";
        case ChessResult::DrawMaterial:   return "DrawMaterial";
        case ChessResult::DrawAgreed:     return "DrawAgreed";
        case ChessResult::WhiteResigns:   return "WhiteResigns";
        case ChessResult::BlackResigns:   return "BlackResigns";
        case ChessResult::Aborted:        return "Aborted";
    }
    return "?";
}

// --- serialisation ----------------------------------------------------------

void chess_pack(const ChessPos *p, uint8_t out[CH_PACKED_SIZE])
{
    memset(out, 0, CH_PACKED_SIZE);
    for (uint8_t s = 0; s < 64; ++s) {
        uint8_t pc = p->board[to88(s)];
        if (s & 1) out[s >> 1] |= (uint8_t)(pc << 4);
        else       out[s >> 1] |= (uint8_t)(pc & 0x0F);
    }
    out[32] = p->stm;
    out[33] = p->castling;
    out[34] = p->ep_file;
    out[35] = p->halfmove;
    out[36] = (uint8_t)(p->fullmove & 0xFF);
    out[37] = (uint8_t)(p->fullmove >> 8);
}

bool chess_unpack(ChessPos *p, const uint8_t in[CH_PACKED_SIZE])
{
    memset(p, 0, sizeof(*p));
    for (uint8_t s = 0; s < 64; ++s) {
        uint8_t pc = (s & 1) ? (uint8_t)(in[s >> 1] >> 4)
                             : (uint8_t)(in[s >> 1] & 0x0F);
        // 7, 8 and 15 are not pieces. Rejecting them here means the rest of
        // the engine never sees a nibble it has no meaning for.
        if (pc == 7 || pc == 8 || pc == 15) return false;
        uint8_t sq = to88(s);
        p->board[sq] = pc;
        if (pc == CH_WK) p->king[CH_WHITE] = sq;
        if (pc == CH_BK) p->king[CH_BLACK] = sq;
    }
    p->stm      = in[32];
    p->castling = in[33];
    p->ep_file  = in[34];
    p->halfmove = in[35];
    p->fullmove = (uint16_t)(in[36] | (in[36 + 1] << 8));
    if (p->fullmove == 0) p->fullmove = 1;
    return chess_pos_valid(p);
}

size_t chess_to_fen(const ChessPos *p, char *out, size_t out_sz)
{
    char buf[100];
    size_t n = 0;
    for (int r = 7; r >= 0; --r) {
        int empty = 0;
        for (int f = 0; f < 8; ++f) {
            uint8_t pc = p->board[r * 16 + f];
            if (pc == CH_EMPTY) { empty++; continue; }
            if (empty) { buf[n++] = (char)('0' + empty); empty = 0; }
            buf[n++] = kPieceChar[pc & 15];
        }
        if (empty) buf[n++] = (char)('0' + empty);
        if (r) buf[n++] = '/';
    }
    buf[n++] = ' ';
    buf[n++] = p->stm == CH_WHITE ? 'w' : 'b';
    buf[n++] = ' ';
    if (!p->castling) {
        buf[n++] = '-';
    } else {
        if (p->castling & CH_WK_SIDE) buf[n++] = 'K';
        if (p->castling & CH_WQ_SIDE) buf[n++] = 'Q';
        if (p->castling & CH_BK_SIDE) buf[n++] = 'k';
        if (p->castling & CH_BQ_SIDE) buf[n++] = 'q';
    }
    buf[n++] = ' ';
    if (p->ep_file > 7) {
        buf[n++] = '-';
    } else {
        buf[n++] = (char)('a' + p->ep_file);
        buf[n++] = p->stm == CH_WHITE ? '6' : '3';
    }
    buf[n] = '\0';

    int extra = snprintf(buf + n, sizeof(buf) - n, " %u %u",
                         (unsigned)p->halfmove, (unsigned)p->fullmove);
    if (extra > 0) n += (size_t)extra;

    if (out_sz == 0) return n;
    size_t copy = n < out_sz - 1 ? n : out_sz - 1;
    memcpy(out, buf, copy);
    out[copy] = '\0';
    return n;
}

bool chess_from_fen(ChessPos *p, const char *fen)
{
    if (!fen) return false;
    memset(p, 0, sizeof(*p));
    p->ep_file  = CH_NO_EP;
    p->fullmove = 1;

    const char *s = fen;
    while (*s == ' ') s++;

    int r = 7, f = 0;
    for (; *s && *s != ' '; ++s) {
        if (*s == '/') {
            if (f != 8) return false;
            if (--r < 0) return false;
            f = 0;
            continue;
        }
        if (*s >= '1' && *s <= '8') {
            f += *s - '0';
            if (f > 8) return false;
            continue;
        }
        uint8_t pc = piece_from_char(*s);
        if (pc == CH_EMPTY || f > 7) return false;
        uint8_t sq = (uint8_t)(r * 16 + f);
        p->board[sq] = pc;
        if (pc == CH_WK) p->king[CH_WHITE] = sq;
        if (pc == CH_BK) p->king[CH_BLACK] = sq;
        f++;
    }
    if (r != 0 || f != 8) return false;

    while (*s == ' ') s++;
    if (*s == 'b') p->stm = CH_BLACK;
    else if (*s == 'w') p->stm = CH_WHITE;
    else return false;
    s++;

    while (*s == ' ') s++;
    if (*s == '-') {
        s++;
    } else {
        for (; *s && *s != ' '; ++s) {
            switch (*s) {
                case 'K': p->castling |= CH_WK_SIDE; break;
                case 'Q': p->castling |= CH_WQ_SIDE; break;
                case 'k': p->castling |= CH_BK_SIDE; break;
                case 'q': p->castling |= CH_BQ_SIDE; break;
                default:  return false;
            }
        }
    }

    while (*s == ' ') s++;
    if (*s == '-') {
        s++;
    } else if (*s >= 'a' && *s <= 'h') {
        p->ep_file = (uint8_t)(*s - 'a');
        s++;
        if (*s >= '1' && *s <= '8') s++;
    }

    // Halfmove and fullmove are optional: the perft suite and hand-typed
    // console positions routinely omit them.
    while (*s == ' ') s++;
    if (*s >= '0' && *s <= '9') {
        unsigned v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned)(*s++ - '0');
        p->halfmove = (uint8_t)(v > 255 ? 255 : v);
        while (*s == ' ') s++;
        if (*s >= '0' && *s <= '9') {
            unsigned w = 0;
            while (*s >= '0' && *s <= '9') w = w * 10 + (unsigned)(*s++ - '0');
            p->fullmove = (uint16_t)(w == 0 ? 1 : (w > 65535 ? 65535 : w));
        }
    }

    return chess_pos_valid(p);
}

void chess_move_to_uci(ChessMove m, char out[6])
{
    if (m == CH_MOVE_NONE) { memcpy(out, "0000", 5); return; }
    uint8_t from = ch_move_from(m), to = ch_move_to(m), promo = ch_move_promo(m);
    out[0] = (char)('a' + (from & 7));
    out[1] = (char)('1' + (from >> 3));
    out[2] = (char)('a' + (to & 7));
    out[3] = (char)('1' + (to >> 3));
    if (promo) {
        out[4] = (char)(kPieceChar[promo] | 0x20);   // lowercase
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

bool chess_move_from_uci(ChessPos *p, const char *s, ChessMove *out)
{
    if (!s || !out) return false;
    if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return false;
    if (s[2] < 'a' || s[2] > 'h' || s[3] < '1' || s[3] > '8') return false;

    uint8_t from = (uint8_t)((s[1] - '1') * 8 + (s[0] - 'a'));
    uint8_t to   = (uint8_t)((s[3] - '1') * 8 + (s[2] - 'a'));
    uint8_t promo = CH_NONE;
    switch (s[4]) {
        case 'q': case 'Q': promo = CH_QUEEN;  break;
        case 'r': case 'R': promo = CH_ROOK;   break;
        case 'b': case 'B': promo = CH_BISHOP; break;
        case 'n': case 'N': promo = CH_KNIGHT; break;
        default: break;
    }

    // Matched against the legal list rather than constructed, so the caller
    // gets a move that is guaranteed playable and the promotion encoding can
    // never disagree with what the generator produces.
    ChessMove buf[64];
    int n = chess_moves_from(p, from, buf, 64);
    for (int i = 0; i < n; ++i) {
        if (ch_move_to(buf[i]) != to) continue;
        if (promo && ch_move_promo(buf[i]) != promo) continue;
        if (!promo && ch_move_promo(buf[i]) != CH_NONE) continue;
        *out = buf[i];
        return true;
    }
    return false;
}

// --- test only --------------------------------------------------------------

uint64_t chess_perft(ChessPos *p, int depth, ChessMove *scratch)
{
    if (depth <= 0) return 1;
    int n = chess_gen_legal(p, scratch, CH_MAX_MOVES);
    if (depth == 1) return (uint64_t)n;

    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
        ChessUndo u;
        if (!chess_make(p, scratch[i], &u)) continue;
        total += chess_perft(p, depth - 1, scratch + CH_MAX_MOVES);
        chess_unmake(p, scratch[i], &u);
    }
    return total;
}
