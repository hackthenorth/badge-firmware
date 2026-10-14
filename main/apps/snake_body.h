#pragma once

#include <cstdint>

namespace Snake {

static constexpr int kCols = 24;
static constexpr int kRows = 16;
static constexpr int kCapacity = kCols * kRows;

struct Cell {
    int x, y;
    bool operator==(Cell other) const { return x == other.x && y == other.y; }
};

inline int cell_index(Cell cell) { return cell.y * kCols + cell.x; }
inline Cell cell_position(int index) { return { index % kCols, index / kCols }; }

class Body {
public:
    void reset()
    {
        tail_ = length_ = 0;
        free_count_ = kCapacity;
        for (int i = 0; i < kCapacity; ++i) {
            free_cells_[i] = i;
            free_slots_[i] = i;
        }
        for (int x = kCols / 2 - 2; x <= kCols / 2; ++x)
            append(cell_index({ x, kRows / 2 }));
    }

    int length() const { return length_; }
    int free_count() const { return free_count_; }
    bool full() const { return length_ == kCapacity; }
    Cell tail() const { return segment(0); }
    Cell head() const { return segment(length_ - 1); }

    // The fixed queue runs from tail to head; only its ends change per step.
    Cell segment(int offset) const
    {
        return cell_position(cells_[(tail_ + offset) % kCapacity]);
    }

    bool advance(Cell next, bool grow)
    {
        if (next.x < 0 || next.x >= kCols || next.y < 0 || next.y >= kRows)
            return false;
        const int cell = cell_index(next);
        const int old_tail = cells_[tail_];
        if (grow && full()) return false;
        if (free_slots_[cell] == kCapacity && (grow || cell != old_tail))
            return false;

        if (!grow) {
            free_slots_[old_tail] = free_count_;
            free_cells_[free_count_++] = old_tail;
            tail_ = (tail_ + 1) % kCapacity;
            --length_;
        }
        append(cell);
        return true;
    }

    Cell free_cell(uint32_t random) const
    {
        if (free_count_ == 0) return { -1, -1 };
        return cell_position(free_cells_[random % free_count_]);
    }

private:
    void append(int cell)
    {
        cells_[(tail_ + length_) % kCapacity] = cell;
        ++length_;

        // Swap-remove from the free-cell pool. Its reverse index also serves
        // as the occupancy map, so collisions and food placement need no scan.
        const int slot = free_slots_[cell];
        const int last = free_cells_[--free_count_];
        free_cells_[slot] = last;
        free_slots_[last] = slot;
        free_slots_[cell] = kCapacity;
    }

    uint16_t cells_[kCapacity] = {};
    uint16_t free_cells_[kCapacity] = {};
    uint16_t free_slots_[kCapacity] = {};
    int tail_ = 0;
    int length_ = 0;
    int free_count_ = 0;
};

} // namespace Snake
