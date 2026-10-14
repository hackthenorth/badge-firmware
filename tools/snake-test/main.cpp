#include "snake_body.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <deque>
#include <random>
#include <vector>

using namespace Snake;

static std::deque<Cell> initial_body()
{
    return {{10, 8}, {11, 8}, {12, 8}};
}

static void check(const Body &body, const std::deque<Cell> &expected)
{
    assert(body.length() == static_cast<int>(expected.size()));
    assert(body.head() == expected.back());
    assert(body.tail() == expected.front());
    assert(body.full() == (body.length() == kCapacity));
    assert(body.free_count() == kCapacity - body.length());

    std::array<bool, kCapacity> seen{};
    for (int i = 0; i < body.length(); ++i) {
        const Cell cell = body.segment(i);
        assert(cell == expected[i]);
        assert(!seen[cell_index(cell)]);
        seen[cell_index(cell)] = true;
    }
    // Every vacant cell is selectable exactly once, even near a full board.
    for (int i = 0; i < body.free_count(); ++i) {
        const Cell cell = body.free_cell(i);
        assert(cell.x >= 0 && cell.x < kCols && cell.y >= 0 && cell.y < kRows);
        assert(!seen[cell_index(cell)]);
        seen[cell_index(cell)] = true;
    }
    assert(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    if (body.full()) assert((body.free_cell(0) == Cell{-1, -1}));
}

static bool advance(Body &body, std::deque<Cell> &expected, Cell next, bool grow)
{
    const auto first = grow ? expected.begin() : expected.begin() + 1;
    const bool allowed = next.x >= 0 && next.x < kCols
        && next.y >= 0 && next.y < kRows
        && (!grow || expected.size() < kCapacity)
        && std::find(first, expected.end(), next) == expected.end();
    assert(body.advance(next, grow) == allowed);
    if (allowed) {
        if (!grow) expected.pop_front();
        expected.push_back(next);
    }
    check(body, expected);
    return allowed;
}

// A cycle through every board cell, including the initial three segments.
static std::vector<Cell> board_cycle()
{
    std::vector<Cell> cycle;
    for (int x = 0; x < kCols; ++x) cycle.push_back({x, 0});
    for (int y = 1; y < kRows; ++y) {
        if (y % 2) {
            for (int x = kCols - 1; x > 0; --x) cycle.push_back({x, y});
        } else {
            for (int x = 1; x < kCols; ++x) cycle.push_back({x, y});
        }
    }
    for (int y = kRows - 1; y > 0; --y) cycle.push_back({0, y});
    assert(cycle.size() == kCapacity);
    return cycle;
}

int main()
{
    Body body;
    body.reset();
    auto expected = initial_body();
    check(body, expected);
    assert(!advance(body, expected, {11, 8}, false)); // Neck collision.
    assert(advance(body, expected, {12, 7}, true));
    assert(advance(body, expected, {11, 7}, false));
    assert(!advance(body, expected, {11, 8}, true)); // Tail stays when growing.
    assert(advance(body, expected, {11, 8}, false)); // Departing tail is legal.
    for (Cell outside : {Cell{-1, 8}, Cell{kCols, 8}, Cell{12, -1}, Cell{12, kRows}})
        assert(!advance(body, expected, outside, false));

    const auto cycle = board_cycle();
    for (int growth_period : {1, 13}) {
        body.reset();
        expected = initial_body();
        int pos = std::find(cycle.begin(), cycle.end(), body.head()) - cycle.begin();
        for (int step = 1; !body.full(); ++step) {
            pos = (pos + 1) % kCapacity;
            assert(advance(body, expected, cycle[pos], step % growth_period == 0));
        }
        assert(!advance(body, expected, body.tail(), true));
        for (int i = 0; i < kCapacity * 3; ++i)
            assert(advance(body, expected, body.tail(), false));
    }

    // Compare a queue reference against mixed moves, deaths and restarts.
    std::mt19937 random(67);
    const Cell directions[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    body.reset();
    expected = initial_body();
    for (int step = 0; step < 25000; ++step) {
        const Cell delta = directions[random() % 4];
        const Cell head = body.head();
        if (!advance(body, expected, {head.x + delta.x, head.y + delta.y},
                     random() % 3 == 0)) {
            body.reset();
            expected = initial_body();
            check(body, expected);
        }
    }
    puts("Snake: collisions, tail reuse, growth, wraparound, full board, food pool, restart and 25,000 reference moves: PASS");
}
