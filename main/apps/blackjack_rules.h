#pragma once

#include <array>
#include <cstdint>

namespace Blackjack {

using Deck = std::array<uint8_t, 52>; // Four suits; ranks A..K within each suit.
enum class Result { Playing, Win, TieWin, Loss };

struct Hand {
    // With a single deck, a live hand can have at most 11 cards before a hit.
    std::array<uint8_t, 12> cards{};
    unsigned count = 0;

    int total() const
    {
        int sum = 0, aces = 0;
        for (unsigned i = 0; i < count; ++i) {
            const int rank = cards[i] % 13 + 1;
            sum += rank == 1 ? 11 : (rank > 10 ? 10 : rank);
            if (rank == 1) ++aces;
        }
        while (sum > 21 && aces-- > 0) sum -= 10;
        return sum;
    }
};

inline Result compare(const Hand &player, const Hand &dealer)
{
    if (player.total() > 21) return Result::Loss;
    if (dealer.total() > 21 || player.total() > dealer.total()) return Result::Win;
    if (player.total() == dealer.total()) return Result::TieWin;
    return Result::Loss;
}

class Round {
public:
    void deal(const Deck &deck)
    {
        deck_ = deck;
        next_ = 0;
        player_ = {};
        dealer_ = {};
        result_ = Result::Playing;
        draw(player_); draw(dealer_);
        draw(player_); draw(dealer_);
        // Peek for natural blackjacks before offering player actions.
        if (player_.total() == 21 || dealer_.total() == 21)
            result_ = compare(player_, dealer_);
    }

    void hit()
    {
        if (result_ != Result::Playing) return;
        draw(player_);
        if (player_.total() > 21) result_ = Result::Loss;
        else if (player_.total() == 21) stand();
    }

    void stand()
    {
        if (result_ != Result::Playing) return;
        // Stand on all 17s, including soft 17 (an ace counted as 11).
        while (dealer_.total() < 17) draw(dealer_);
        result_ = compare(player_, dealer_);
    }

    const Hand &player() const { return player_; }
    const Hand &dealer() const { return dealer_; }
    Result result() const { return result_; }

private:
    void draw(Hand &hand) { hand.cards[hand.count++] = deck_[next_++]; }

    Deck deck_{};
    unsigned next_ = 0;
    Hand player_, dealer_;
    Result result_ = Result::Loss;
};

} // namespace Blackjack
