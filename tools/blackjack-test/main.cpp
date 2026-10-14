#include "blackjack_rules.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <random>

using namespace Blackjack;

// Put requested ranks first, then the remaining unique cards of a real deck.
static Deck deck(std::initializer_list<int> ranks)
{
    Deck cards{};
    bool used[52] = {};
    unsigned next = 0;
    for (int rank : ranks) {
        unsigned card = rank - 1;
        while (card < 52 && used[card]) card += 13;
        assert(card < 52);
        cards[next++] = card;
        used[card] = true;
    }
    for (unsigned card = 0; card < 52; ++card)
        if (!used[card]) cards[next++] = card;
    assert(next == 52);
    return cards;
}

int main()
{
    Hand aces{{0, 13, 8, 9}, 4}; // A, A, 9, 10: both aces become 1.
    assert(aces.total() == 21);
    Hand faces{{10, 11, 12}, 3};
    assert(faces.total() == 30);
    assert(compare(faces, faces) == Result::Loss); // A bust never wins a tie.

    Round round;
    round.deal(deck({10, 10, 8, 8}));
    assert(round.result() == Result::Playing);
    round.stand();
    assert(round.result() == Result::TieWin);
    round.hit(); round.stand(); // Finished hands cannot change or draw again.
    assert(round.result() == Result::TieWin && round.player().count == 2);

    round.deal(deck({1, 1, 13, 12}));
    assert(round.result() == Result::TieWin); // Both natural blackjacks.
    round.deal(deck({1, 10, 13, 9}));
    assert(round.result() == Result::Win);
    round.deal(deck({10, 1, 9, 13}));
    assert(round.result() == Result::Loss);

    round.deal(deck({10, 10, 6, 7, 10}));
    round.hit();
    assert(round.result() == Result::Loss && round.player().total() == 26);
    assert(round.dealer().count == 2); // Dealer doesn't draw after a player bust.

    round.deal(deck({10, 10, 8, 6, 10}));
    round.stand();
    assert(round.result() == Result::Win && round.dealer().total() == 26);
    round.deal(deck({10, 1, 6, 6}));
    round.stand();
    assert(round.result() == Result::Loss && round.dealer().count == 2); // Soft 17.

    round.deal(deck({10, 10, 6, 6, 5, 5}));
    round.hit(); // Reaching 21 auto-stands; dealer also reaches 21.
    assert(round.result() == Result::TieWin);

    // Exercise complete rounds under ASan/UBSan, including long ace-heavy hands.
    std::mt19937 random(67);
    auto cards = deck({});
    for (int i = 0; i < 10000; ++i) {
        std::shuffle(cards.begin(), cards.end(), random);
        round.deal(cards);
        while (round.result() == Result::Playing) {
            if (round.player().total() < (i % 2 ? 21 : 17)) round.hit();
            else round.stand();
        }
        assert(round.result() == compare(round.player(), round.dealer()));
        assert(round.player().count <= 12 && round.dealer().count <= 12);
    }
    puts("Blackjack: ace scoring, ties, naturals, busts, soft 17, replay and 10,000 rounds: PASS");
}
