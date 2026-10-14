#pragma once

#include <cstdint>
#include <functional>

enum class Button : uint8_t {
    // First 8 (0..7) are read from the 74HC165 shift register, in shift order.
    A = 0, B, Home, Down, Left, Right, Up, Aux1,
    // Start is a dedicated pushbutton on its own GPIO (PIN_BTN_START), not the HC165.
    Start,
    COUNT
};

enum class ButtonEventKind : uint8_t { Pressed, Released };

struct ButtonEvent {
    Button button;
    ButtonEventKind kind;
};

void hal_buttons_init();
void hal_buttons_set_dispatcher(std::function<void(ButtonEvent)> fn);

// Inject a synthetic press/release pair into the same queue the HC165 poll
// task feeds — used by the console `press` command and other dev tools.
void hal_buttons_inject_press(Button b);

// Case-insensitive lookup of a button by its serial-protocol name
// ("A", "B", "Home", "Down", ..., "Aux1", "Start"). Returns false on unknown names.
bool hal_buttons_button_from_name(const char *name, Button *out);
