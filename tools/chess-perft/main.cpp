// Host test harness for main/apps/chess_rules.cpp.
//
// The rules engine is deliberately free of LVGL and ESP-IDF so it can be
// verified here, with plain g++, in under a second — instead of on a badge,
// behind a multi-minute IDF build, through a serial console. Perft is the
// standard test: it counts leaf nodes of the legal move tree to a given depth,
// and the published counts for these six positions are exact. A single
// mis-generated or mis-filtered move anywhere in the tree changes the total, so
// matching all of them is strong evidence the rules are right.
//
//   make && ./perft

#include "chess_rules.h"
#include "mp_proto.h"

#include <chrono>
#include <cstdio>
#include <cstring>

static int g_fail = 0;
static int g_pass = 0;

static void ok(bool cond, const char *what)
{
    if (cond) { g_pass++; return; }
    g_fail++;
    printf("  FAIL  %s\n", what);
}

// Perft needs CH_MAX_MOVES entries per level of depth.
static ChessMove g_scratch[CH_MAX_MOVES * 8];

struct PerftCase {
    const char *name;
    const char *fen;
    int         max_depth;
    uint64_t    expect[6];   // expect[d-1] is the count at depth d
};

// The standard suite (chessprogramming.org "Perft Results"). Positions 2-6 are
// chosen precisely because each one breaks a different naive generator:
// Kiwipete is dense with pins and castling, position 3 is the en-passant
// discovered-check trap, position 4 is promotion under check.
static const PerftCase kCases[] = {
    { "startpos",
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      5, { 20, 400, 8902, 197281, 4865609, 0 } },
    { "kiwipete",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
      4, { 48, 2039, 97862, 4085603, 0, 0 } },
    { "position 3",
      "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
      5, { 14, 191, 2812, 43238, 674624, 0 } },
    { "position 4",
      "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
      4, { 6, 264, 9467, 422333, 0, 0 } },
    { "position 5",
      "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
      4, { 44, 1486, 62379, 2103487, 0, 0 } },
    { "position 6",
      "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
      4, { 46, 2079, 89890, 3894594, 0, 0 } },
};

// --- serialisation round trips ----------------------------------------------

// Walks the same tree perft does, but at every node packs the position to the
// 38-byte wire form, unpacks it, and demands an identical Zobrist key and FEN.
// Running it inside the move tree rather than over a handful of hand-picked
// positions gets the round trip exercised over millions of real positions —
// including every castling-rights and en-passant state the generator can
// produce — for the cost of one extra function.
static uint64_t verify_walk(ChessPos *p, int depth, ChessMove *scratch)
{
    uint8_t packed[CH_PACKED_SIZE];
    ChessPos back;
    char fen_a[100], fen_b[100];

    chess_pack(p, packed);
    if (!chess_unpack(&back, packed)) {
        g_fail++;
        chess_to_fen(p, fen_a, sizeof(fen_a));
        printf("  FAIL  unpack rejected a position we packed: %s\n", fen_a);
        return 0;
    }
    if (chess_hash(&back) != chess_hash(p)) {
        g_fail++;
        chess_to_fen(p, fen_a, sizeof(fen_a));
        printf("  FAIL  pack/unpack changed the hash: %s\n", fen_a);
    }
    chess_to_fen(p, fen_a, sizeof(fen_a));
    chess_to_fen(&back, fen_b, sizeof(fen_b));
    if (strcmp(fen_a, fen_b) != 0) {
        g_fail++;
        printf("  FAIL  pack/unpack changed the FEN:\n        %s\n        %s\n",
               fen_a, fen_b);
    }

    // FEN round trip on the same node, for free.
    ChessPos from_text;
    if (!chess_from_fen(&from_text, fen_a)) {
        g_fail++;
        printf("  FAIL  from_fen rejected our own to_fen: %s\n", fen_a);
    } else if (chess_hash(&from_text) != chess_hash(p)) {
        g_fail++;
        printf("  FAIL  FEN round trip changed the hash: %s\n", fen_a);
    }

    if (depth <= 0) return 1;
    int n = chess_gen_legal(p, scratch, CH_MAX_MOVES);
    uint64_t total = 0;
    for (int i = 0; i < n; ++i) {
        ChessUndo u;
        chess_make(p, scratch[i], &u);
        total += verify_walk(p, depth - 1, scratch + CH_MAX_MOVES);
        chess_unmake(p, scratch[i], &u);
    }
    return depth == 0 ? 1 : total;
}

// --- make/unmake symmetry ---------------------------------------------------

// chess_unmake() must restore the position bit for bit, or perft's counts are
// meaningless and every legality filter silently corrupts the board. Compare
// the whole struct, not just the hash: the hash does not cover halfmove,
// fullmove or the cached king squares.
static void check_unmake_restores(const char *fen)
{
    ChessPos p;
    if (!chess_from_fen(&p, fen)) { ok(false, "unmake fixture FEN parsed"); return; }

    ChessMove moves[CH_MAX_MOVES];
    int n = chess_gen_legal(&p, moves, CH_MAX_MOVES);
    for (int i = 0; i < n; ++i) {
        ChessPos before = p;
        ChessUndo u;
        chess_make(&p, moves[i], &u);
        chess_unmake(&p, moves[i], &u);
        if (memcmp(&before, &p, sizeof(ChessPos)) != 0) {
            char m[6];
            chess_move_to_uci(moves[i], m);
            g_fail++;
            printf("  FAIL  unmake did not restore after %s in %s\n", m, fen);
            return;
        }
    }
    g_pass++;
}

// --- targeted rule tests ----------------------------------------------------

static bool has_move(const char *fen, const char *uci)
{
    ChessPos p;
    if (!chess_from_fen(&p, fen)) return false;
    ChessMove m;
    return chess_move_from_uci(&p, uci, &m);
}

static ChessResult play_line(const char *fen, const char *const *moves, int count)
{
    ChessGame g;
    ChessPos p;
    if (!chess_from_fen(&p, fen)) return ChessResult::Aborted;
    chess_game_reset(&g);
    if (!chess_game_adopt(&g, &p, false)) return ChessResult::Aborted;
    for (int i = 0; i < count; ++i) {
        ChessMove m;
        if (!chess_move_from_uci(&g.pos, moves[i], &m)) {
            printf("  FAIL  illegal move %s in line\n", moves[i]);
            g_fail++;
            return ChessResult::Aborted;
        }
        if (!chess_game_play(&g, m)) {
            printf("  FAIL  chess_game_play rejected %s\n", moves[i]);
            g_fail++;
            return ChessResult::Aborted;
        }
    }
    return g.result;
}

static void rule_tests()
{
    printf("rules\n");

    // Back-rank mate: Ra1-a8 with the black king boxed in by its own pawns.
    {
        const char *line[] = { "a1a8" };
        ok(play_line("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", line, 1)
               == ChessResult::WhiteMates, "back-rank mate is WhiteMates");
    }

    // Stalemate: Kh8 has no legal move and is not in check.
    {
        ChessGame g;
        ChessPos p;
        ok(chess_from_fen(&p, "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"), "stalemate FEN");
        chess_game_reset(&g);
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::Stalemate, "stalemate detected");
    }

    // The en-passant discovered check. Both pawns leave the fifth rank, which
    // exposes Ka5 to Rh5 — so dxc6 is illegal even though it looks like an
    // ordinary capture. This is the case a "pinned piece" shortcut gets wrong
    // and generic make/unmake gets right for free.
    {
        const char *fen = "8/8/8/K1pP3r/8/8/8/7k w - c6 0 1";
        ok(!has_move(fen, "d5c6"), "en passant exposing the king is rejected");
        ok(has_move(fen, "d5d6"), "the ordinary pawn push is still legal");
    }

    // A legal en passant capture, for the converse.
    ok(has_move("8/8/8/2pP4/8/8/8/K6k w - c6 0 1", "d5c6"),
       "legal en passant is generated");

    // Castling: both sides, both wings, from the standard empty-back-rank test
    // position. 26 is the published perft(1).
    {
        ChessPos p;
        ChessMove moves[CH_MAX_MOVES];
        ok(chess_from_fen(&p, "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"), "castle FEN");
        int n = chess_gen_legal(&p, moves, CH_MAX_MOVES);
        ok(n == 26, "castling position has 26 legal moves");
        ok(has_move("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1"), "O-O generated");
        ok(has_move("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1c1"), "O-O-O generated");
    }

    // Castling through an attacked square is illegal even though the
    // destination is safe — the rule the generic legality filter cannot see.
    ok(!has_move("4k3/8/8/8/8/8/8/R3K2r w Q - 0 1", "e1c1"),
       "cannot castle out of check");
    ok(!has_move("4k3/8/8/8/8/5r2/8/4K2R w K - 0 1", "e1g1"),
       "cannot castle through an attacked square");

    // Promotion, including under-promotion.
    {
        ChessPos p;
        ChessMove moves[CH_MAX_MOVES];
        ok(chess_from_fen(&p, "8/4P3/8/8/8/8/8/K6k w - - 0 1"), "promotion FEN");
        int n = chess_moves_from(&p, 52 /* e7 */, moves, CH_MAX_MOVES);
        ok(n == 4, "a promoting pawn generates four moves");
        ok(has_move("8/4P3/8/8/8/8/8/K6k w - - 0 1", "e7e8n"),
           "under-promotion to a knight is offered");
    }

    // Threefold repetition: knights out and back, twice.
    {
        const char *line[] = { "g1f3", "g8f6", "f3g1", "f6g8",
                               "g1f3", "g8f6", "f3g1", "f6g8" };
        ok(play_line("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                     line, 8) == ChessResult::DrawRepetition,
           "threefold repetition ends the game");
    }

    // An irreversible move must clear the history, so a position reached
    // before a pawn move can never count toward a later repetition.
    {
        ChessGame g;
        ChessPos p;
        chess_from_fen(&p, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        chess_game_reset(&g);
        chess_game_adopt(&g, &p, false);
        ChessMove m;
        chess_move_from_uci(&g.pos, "e2e4", &m);
        chess_game_play(&g, m);
        ok(g.hist_len == 1, "a pawn move resets repetition history");
    }

    // Fifty-move rule: one reversible move takes halfmove from 99 to 100.
    {
        const char *line[] = { "a1a2" };
        ok(play_line("7k/8/8/8/8/8/8/R6K w - - 99 60", line, 1)
               == ChessResult::DrawFifty, "the fifty-move rule fires at 100 plies");
    }

    // Dead positions.
    {
        ChessGame g;
        ChessPos p;
        chess_from_fen(&p, "8/8/8/4k3/8/8/8/4K3 w - - 0 1");
        chess_game_reset(&g);
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::DrawMaterial, "K vs K is a dead position");

        chess_from_fen(&p, "8/8/8/4k3/8/8/5B2/4K3 w - - 0 1");
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::DrawMaterial, "K+B vs K is a dead position");

        chess_from_fen(&p, "8/8/8/4k3/8/8/5N2/4K3 w - - 0 1");
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::DrawMaterial, "K+N vs K is a dead position");

        // Two knights can mate with cooperation, so FIDE does not call it dead.
        chess_from_fen(&p, "8/8/8/4k3/8/8/4NN2/4K3 w - - 0 1");
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::Playing, "K+NN vs K is still playable");

        // Bishops on opposite colours can mate; same colour cannot.
        chess_from_fen(&p, "8/8/8/4k3/8/8/2b2B2/4K3 w - - 0 1");
        chess_game_adopt(&g, &p, false);
        ok(g.result == ChessResult::Playing, "opposite-colour bishops are playable");
    }

    // A position the wire must refuse: the side not to move standing in check
    // is unreachable by any legal move, so it is corruption, not a position.
    {
        ChessPos p;
        ok(!chess_from_fen(&p, "4k3/8/8/8/8/8/8/r3K3 b - - 0 1"),
           "a position with the wrong side in check is rejected");
    }

    // Invalid piece nibbles must not survive unpack.
    {
        uint8_t packed[CH_PACKED_SIZE];
        ChessPos p;
        chess_start_pos(&p);
        chess_pack(&p, packed);
        packed[0] = 0x07;   // nibble 7 on a1
        ok(!chess_unpack(&p, packed), "unpack rejects piece nibble 7");
        chess_start_pos(&p);
        chess_pack(&p, packed);
        packed[0] = (uint8_t)((packed[0] & 0x0F) | 0xF0);   // nibble 15 on b1
        ok(!chess_unpack(&p, packed), "unpack rejects piece nibble 15");
    }

    // Square numbering, asserted directly rather than assumed. a1=0, h8=63.
    {
        ChessPos p;
        chess_start_pos(&p);
        ok(chess_piece_at(&p, 0) == CH_WR,  "a1 is square 0");
        ok(chess_piece_at(&p, 4) == CH_WK,  "e1 is square 4");
        ok(chess_piece_at(&p, 60) == CH_BK, "e8 is square 60");
        ok(chess_piece_at(&p, 63) == CH_BR, "h8 is square 63");
        char uci[6];
        chess_move_to_uci(ch_move(12, 28), uci);
        ok(strcmp(uci, "e2e4") == 0, "move 12->28 is e2e4");
    }
}

// --- wire format ------------------------------------------------------------

static void proto_tests()
{
    printf("wire format\n");

    // The standard CRC-16/CCITT-FALSE check value. If this is wrong, two
    // badges built from different revisions of the header would reject each
    // other's frames as corrupt with no other symptom.
    ok(mpg_crc16("123456789", 9) == 0x29B1, "CRC-16/CCITT-FALSE check value");
    ok(mpg_crc16("", 0) == 0xFFFF, "CRC-16 of an empty body is the init value");

    // Sizes are static_asserts in the header, but printing them makes a
    // regression legible rather than just a failed build.
    ok(sizeof(mpg_hdr_t) == 38, "header is 38 bytes");
    ok(sizeof(mpg_chess_body_t) == 44, "chess body is 44 bytes");
    ok(MPG_CHESS_FRAME == 82, "a chess frame is 82 bytes");
    printf("  frame %d + AD %d = %d on air (TX cap %d, RX cap %d)\n",
           MPG_CHESS_FRAME, MPG_AD_OVERHEAD, MPG_CHESS_FRAME + MPG_AD_OVERHEAD,
           MPG_HCI_MAX_ADV_DATA, MPG_HCI_MAX_ADV_RPT);

    // chess_pack() writes the layout that mpg_pos_t describes. Reading its
    // output back through the wire struct's own accessors is what proves the
    // two agree about nibble order — a static_assert only pins the size.
    {
        ChessPos p;
        chess_start_pos(&p);
        mpg_pos_t wire;
        chess_pack(&p, (uint8_t *)&wire);

        bool all = true;
        for (uint8_t s = 0; s < 64; ++s)
            if (mpg_pos_get(&wire, s) != chess_piece_at(&p, s)) all = false;
        ok(all, "mpg_pos_get agrees with chess_pack on all 64 squares");

        // Spot-check the nibble halves explicitly, so an even/odd swap cannot
        // hide behind a symmetric start position.
        ok(mpg_pos_get(&wire, 0) == CH_WR, "a1 (even square, low nibble) is a rook");
        ok(mpg_pos_get(&wire, 1) == CH_WN, "b1 (odd square, high nibble) is a knight");
        ok(wire.stm == CH_WHITE, "wire stm");
        ok(wire.castling == (CH_WK_SIDE | CH_WQ_SIDE | CH_BK_SIDE | CH_BQ_SIDE),
           "wire castling rights");
        ok(wire.ep_file == CH_NO_EP, "wire ep_file");
        ok(wire.fullmove == 1, "wire fullmove");

        // mpg_pos_set is the inverse.
        mpg_pos_t built;
        memset(&built, 0, sizeof(built));
        for (uint8_t s = 0; s < 64; ++s) mpg_pos_set(&built, s, chess_piece_at(&p, s));
        ok(memcmp(built.squares, wire.squares, 32) == 0,
           "mpg_pos_set round-trips against chess_pack");
    }

    // A whole frame, built and parsed the way the radio path will: memcpy
    // straight in and out of a byte buffer, no serialisation step.
    {
        ChessGame g;
        chess_game_reset(&g);
        ChessMove m;
        chess_move_from_uci(&g.pos, "e2e4", &m);
        chess_game_play(&g, m);

        uint8_t frame[MPG_MAX_FRAME];
        mpg_hdr_t hdr;
        mpg_chess_body_t body;

        memset(&hdr, 0, sizeof(hdr));
        memset(&body, 0, sizeof(body));
        chess_pack(&g.pos, (uint8_t *)&body.pos);
        body.last_move = m;
        body.result    = (uint8_t)g.result;
        body.rep_count = (uint8_t)chess_repetitions(&g);
        body.in_check  = chess_in_check(&g.pos, g.pos.stm) ? 1 : 0;

        mpg_set_magic(&hdr);
        hdr.ver      = MPG_VERSION;
        hdr.type     = MPG_TYPE_STATE;
        hdr.game     = MPG_GAME_CHESS;
        hdr.session  = 0xDEADBEEF;
        hdr.state    = MPG_STATE_LIVE;
        hdr.seat     = 0;
        hdr.seq      = 1;
        hdr.body_len = sizeof(body);
        hdr.sum      = mpg_crc16(&body, sizeof(body));

        memcpy(frame, &hdr, sizeof(hdr));
        memcpy(frame + sizeof(hdr), &body, sizeof(body));
        size_t frame_len = sizeof(hdr) + sizeof(body);
        ok(frame_len == (size_t)MPG_CHESS_FRAME, "built frame is MPG_CHESS_FRAME long");

        // Receive side, in the order mp_session's callback will use it.
        mpg_hdr_t rx;
        memcpy(&rx, frame, sizeof(rx));
        ok(mpg_magic_ok(&rx), "magic survives the round trip");
        ok(rx.ver == MPG_VERSION, "version survives");
        ok(rx.session == 0xDEADBEEF, "session survives (LE, 4-aligned)");
        ok(rx.seq == 1, "seq survives");
        ok(MPG_HDR_SIZE + rx.body_len == frame_len,
           "body_len is consistent with the frame length");

        mpg_chess_body_t rxb;
        memcpy(&rxb, frame + MPG_HDR_SIZE, rx.body_len);
        ok(mpg_crc16(&rxb, rx.body_len) == rx.sum, "body CRC matches");

        ChessPos back;
        ok(chess_unpack(&back, (const uint8_t *)&rxb.pos), "received position unpacks");
        ok(chess_hash(&back) == chess_hash(&g.pos), "received position is identical");
        ok(rxb.last_move == m, "last_move survives");

        char uci[6];
        chess_move_to_uci(rxb.last_move, uci);
        ok(strcmp(uci, "e2e4") == 0, "last_move decodes back to e2e4");

        // A single flipped bit anywhere in the body must fail the CRC. This is
        // the check that stands between us and adopting a corrupt board.
        int caught = 0;
        for (size_t i = 0; i < sizeof(rxb); ++i) {
            mpg_chess_body_t bad = rxb;
            ((uint8_t *)&bad)[i] ^= 0x01;
            if (mpg_crc16(&bad, sizeof(bad)) != rx.sum) caught++;
        }
        ok(caught == (int)sizeof(rxb), "CRC catches a flipped bit in every body byte");
    }

    // Frames from the other protocols sharing company ID 0xFFFF must be
    // rejected on the magic alone, before anything reads a length or a field.
    {
        mpg_hdr_t h;
        memcpy(&h, "BMP2", 4);
        ok(!mpg_magic_ok(&h), "a Badge Bump frame is not mistaken for MPG1");
        memcpy(&h, "BBX1", 4);
        ok(!mpg_magic_ok(&h), "a Solana Scavenger Hunt frame is not mistaken for MPG1");
    }
}

// --- main -------------------------------------------------------------------

int main(int argc, char **argv)
{
    bool quick = (argc > 1 && strcmp(argv[1], "--quick") == 0);

    printf("perft\n");
    for (const auto &c : kCases) {
        ChessPos p;
        if (!chess_from_fen(&p, c.fen)) {
            printf("  FAIL  %s: FEN did not parse\n", c.name);
            g_fail++;
            continue;
        }
        int max_depth = quick ? (c.max_depth > 3 ? 3 : c.max_depth) : c.max_depth;
        for (int d = 1; d <= max_depth; ++d) {
            auto t0 = std::chrono::steady_clock::now();
            uint64_t got = chess_perft(&p, d, g_scratch);
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            uint64_t want = c.expect[d - 1];
            bool good = (got == want);
            if (good) g_pass++; else g_fail++;
            printf("  %-4s %-12s d%d  %10llu  %8.1f ms%s\n",
                   good ? "ok" : "FAIL", c.name, d,
                   (unsigned long long)got, ms,
                   good ? "" : "   <-- expected mismatch");
            if (!good)
                printf("        expected %llu\n", (unsigned long long)want);
        }
    }

    printf("\nmake/unmake symmetry\n");
    for (const auto &c : kCases) check_unmake_restores(c.fen);
    printf("  %d positions checked\n", (int)(sizeof(kCases) / sizeof(kCases[0])));

    printf("\npack / unpack / FEN round trips\n");
    for (const auto &c : kCases) {
        ChessPos p;
        if (!chess_from_fen(&p, c.fen)) continue;
        int before = g_fail;
        uint64_t nodes = verify_walk(&p, quick ? 2 : 3, g_scratch);
        printf("  %-4s %-12s %10llu positions\n",
               g_fail == before ? "ok" : "FAIL", c.name,
               (unsigned long long)nodes);
    }

    printf("\n");
    proto_tests();

    printf("\n");
    rule_tests();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
