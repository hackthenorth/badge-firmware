#pragma once

// LCD (SPI2)
#define PIN_LCD_MOSI 10
#define PIN_LCD_CLK   1
#define PIN_LCD_CS    2
#define PIN_LCD_DC    0
#define PIN_LCD_RST   4

// Shared I2C bus (accel 0x19, NFC 0x26)
#define PIN_I2C_SDA   5
#define PIN_I2C_SCL   6

// 74HC165 buttons
#define PIN_HC165_DATA 7
#define PIN_HC165_LOAD 20
#define PIN_HC165_CLK  21

// Dedicated Start pushbutton (active-low, standard pushbutton, not on the HC165)
#define PIN_BTN_START  9

// WS2812 strip
#define PIN_LED_DIN    3
