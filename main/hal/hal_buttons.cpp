#include "hal_buttons.h"
#include "button_debouncer.h"
#include "pins.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "lvgl.h"

#include <strings.h>

static const char *TAG = "hal_buttons";

#define POLL_INTERVAL_MS 10
#define DRAIN_INTERVAL_MS 10
#define SHIFT_DELAY_US    1

static QueueHandle_t s_queue = nullptr;
static std::function<void(ButtonEvent)> s_dispatcher;
static lv_timer_t *s_drain_timer = nullptr;

static const char *kButtonNames[(uint8_t)Button::COUNT] = {
    "A", "B", "Home", "Down", "Left", "Right", "Up", "Aux1", "Start",
};

static uint8_t hc165_read()
{
    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 0);
    esp_rom_delay_us(SHIFT_DELAY_US);
    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 1);
    esp_rom_delay_us(SHIFT_DELAY_US);

    uint8_t byte = 0;
    for (int bit = 7; bit >= 0; bit--) {
        if (gpio_get_level((gpio_num_t)PIN_HC165_DATA)) {
            byte |= (1 << bit);
        }
        gpio_set_level((gpio_num_t)PIN_HC165_CLK, 1);
        esp_rom_delay_us(SHIFT_DELAY_US);
        gpio_set_level((gpio_num_t)PIN_HC165_CLK, 0);
        esp_rom_delay_us(SHIFT_DELAY_US);
    }
    return byte;
}

static uint16_t pressed_buttons_read()
{
    const uint8_t shifted = hc165_read();
    uint16_t pressed = 0;
    for (uint8_t index = 0; index < 8; ++index) {
        const uint8_t bit = static_cast<uint8_t>(7 - index);
        if (!(shifted & (1u << bit))) pressed |= (1u << index);
    }
    if (!gpio_get_level((gpio_num_t)PIN_BTN_START)) {
        pressed |= (1u << static_cast<uint8_t>(Button::Start));
    }
    return pressed;
}

static void button_poll_task(void *)
{
    // Establish the actual power-on levels as the baseline. In particular,
    // Aux1 is a maintained SideSwitch, so assuming every input starts released
    // turns one of its normal resting positions into a synthetic Pressed event.
    // That queued boot event used to skip the onboarding welcome screen.
    ButtonDebouncer debouncer(pressed_buttons_read());

    while (true) {
        const uint16_t changed = debouncer.update(pressed_buttons_read());
        const uint16_t stable = debouncer.stable_pressed();
        for (uint8_t index = 0; index < static_cast<uint8_t>(Button::COUNT);
             ++index) {
            const uint16_t bit = static_cast<uint16_t>(1u << index);
            if (!(changed & bit)) continue;
            ButtonEvent ev = {
                .button = static_cast<Button>(index),
                .kind = (stable & bit) ? ButtonEventKind::Pressed
                                       : ButtonEventKind::Released,
            };
            xQueueSend(s_queue, &ev, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void hal_buttons_inject_press(Button b)
{
    if (!s_queue) return;
    ButtonEvent press   = { b, ButtonEventKind::Pressed };
    ButtonEvent release = { b, ButtonEventKind::Released };
    xQueueSend(s_queue, &press, 0);
    xQueueSend(s_queue, &release, 0);
}

bool hal_buttons_button_from_name(const char *name, Button *out)
{
    if (!name || !out) return false;
    for (uint8_t i = 0; i < (uint8_t)Button::COUNT; i++) {
        if (strcasecmp(name, kButtonNames[i]) == 0) {
            *out = (Button)i;
            return true;
        }
    }
    return false;
}

static void drain_timer_cb(lv_timer_t *)
{
    ButtonEvent ev;
    while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) {
        if (s_dispatcher) s_dispatcher(ev);
    }
}

void hal_buttons_init()
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_HC165_CLK) | (1ULL << PIN_HC165_LOAD),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_HC165_DATA) | (1ULL << PIN_BTN_START),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_conf);

    gpio_set_level((gpio_num_t)PIN_HC165_LOAD, 1);
    gpio_set_level((gpio_num_t)PIN_HC165_CLK, 0);

    s_queue = xQueueCreate(32, sizeof(ButtonEvent));
    configASSERT(s_queue);

    // lv_timer must be created under the LVGL lock.
    s_drain_timer = lv_timer_create(drain_timer_cb, DRAIN_INTERVAL_MS, nullptr);

    xTaskCreate(button_poll_task, "btn_poll", 4096, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "buttons ready");
}

void hal_buttons_set_dispatcher(std::function<void(ButtonEvent)> fn)
{
    s_dispatcher = std::move(fn);
}
