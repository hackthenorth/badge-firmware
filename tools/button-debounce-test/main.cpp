#include "button_debouncer.h"

#include <cassert>
#include <cstdio>

int main()
{
    ButtonDebouncer buttons(0);

    // One noisy sample is ignored; the second matching sample accepts it.
    assert(buttons.update(1u << 0) == 0);
    assert(buttons.stable_pressed() == 0);
    assert(buttons.update(1u << 0) == (1u << 0));
    assert(buttons.stable_pressed() == (1u << 0));

    // A release bounce back to pressed must not emit either edge.
    assert(buttons.update(0) == 0);
    assert(buttons.update(1u << 0) == 0);
    assert(buttons.stable_pressed() == (1u << 0));
    assert(buttons.update(0) == 0);
    assert(buttons.update(0) == (1u << 0));
    assert(buttons.stable_pressed() == 0);

    // Buttons debounce independently, including the dedicated Start bit.
    constexpr uint16_t together = (1u << 3) | (1u << 8);
    assert(buttons.update(together) == 0);
    assert(buttons.update(together) == together);
    assert(buttons.stable_pressed() == together);

    std::puts("Buttons: 10 ms sampling with two-sample debounce: PASS");
}
