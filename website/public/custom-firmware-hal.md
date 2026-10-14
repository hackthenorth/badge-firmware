# Custom firmware + HAL guide (2026 Hacker Badge)

Target: hackers who want to fully re-flash the badge and vibecode their own
hardware-abstraction layer (HAL). Hardware design files are not published
yet — everything you need is below.

> EXPERIMENTAL: re-flashing replaces the event firmware. If it goes wrong,
> your badge may stop working, and replacements are limited. All bets are off —
> back up anything you care about first.

## 0. What you are working with

- MCU module: ESP32-C3-MINI-1-N4 (RISC-V, 4 MB flash, native USB-Serial-JTAG).
- Display: ST7789, 320x240, RGB565, SPI.
- Buttons: 8x inputs on a 74HC165 shift register + 1 dedicated Start button.
- Accelerometer: SC7A20HTR on shared I2C, address `0x19`.
- NFC reader: MFRC522 on the same I2C bus, address `0x26`.
- LEDs: 6x WS2812B-2020 around the board edge.
- Power: AA battery holder -> MT3608 boost -> XC6220 LDO; USB-C for flashing.
- Flash layout: app at `0x10000` (`0x2A0000`), storage after it
  (`0x140000`). Flash mode DIO, 80 MHz, 4 MB.

## 1. Toolchain + flashing

1. Install ESP-IDF **v5.5.3** (this badge's pinned version) and activate it:
   ```sh
   . ~/.espressif/tools/activate_idf_v5.5.3.sh && idf.py --version
   ```
   (`idf.py` is only on PATH after sourcing that script.)
2. Create an `esp32c3` project (`idf.py create-project` or the ESP-IDF
   "hello world" template) and set the target: `idf.py set-target esp32c3`.
3. Plug in USB-C. The console is **USB-Serial-JTAG, not UART0** — enable
   `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` in menuconfig.
4. Flash: `idf.py -p /dev/tty.usbmodemXXXX flash` (macOS) or `/dev/ttyACM0`
   (Linux). If esptool reports "No serial data received", enter download mode:
   **hold Start (GPIO9 strapping pin) while plugging in USB**, then retry.
   Blank screen in this state is expected — that is download mode, not a brick.
   Native USB has no auto-reset, so the explicit-hold step is normal.
5. `idf.py monitor` to see serial output. Bare `\r` is the console line ending;
   only one process may own the port at a time.
6. Arduino-ESP32 also works (same GPIOs), but every register address and pin
   below is verified against the ESP-IDF firmware, so ESP-IDF is the path of
   least resistance.

## 2. Pin map

| Function | GPIO | Notes |
|---|---|---|
| LCD MOSI | 10 | SPI2, 40 MHz, mode 0 |
| LCD CLK | 1 | |
| LCD CS | 2 | |
| LCD DC | 0 | |
| LCD RST | 4 | |
| I2C SDA | 5 | Shared: accel `0x19` + NFC `0x26`, 400 kHz |
| I2C SCL | 6 | |
| HC165 DATA (MISO-ish) | 7 | Button shift register serial out |
| HC165 LOAD (latch) | 20 | Pulse low to latch |
| HC165 CLK | 21 | |
| START button | 9 | Active-low pushbutton, own GPIO; **strapping pin** (download mode) |
| LED DIN (WS2812) | 3 | RMT peripheral, GRB order |

## 3. Buttons (74HC165 + Start)

- 8 shift-register inputs, shift order: `A, B, Home, Down, Left, Right, Up,
  Aux1` (A shifts out first, Aux1 last). All **active-low**.
- Read protocol (bit-bang, ~1 us delays are fine):
  1. Pull `LOAD` low, then high (latches all 8 inputs).
  2. Repeat 8x: sample `DATA`, then pulse `CLK` high-low.
  3. A sampled `0` = pressed.
- `Start` is separate: `gpio_get_level(9) == 0` = pressed.
- Poll at ~10 ms and debounce. Note Aux1 is a maintained side switch, not
  momentary — don't assume it boots released.
- Gotcha: GPIO9 is a strapping pin. Your button read is fine, but holding Start
  at reset/USB-plug enters download mode (that's the feature above, not a bug).

## 4. Screen (ST7789, 320x240)

- SPI2 host: MOSI=10, CLK=1, CS=2, DC=0, RST=4. 40 MHz, SPI mode 0, 16-bit
  RGB565 pixels.
- Init sequence: reset, init, `invert_color(true)`, `swap_xy(true)`,
  `mirror(true, false)`. If your image appears rotated/mirrored, those three
  calls are the fix.
- Easiest path: ESP-IDF `esp_lcd` ST7789 driver
  (`esp_lcd_new_panel_st7789`) + `esp_lvgl_port` if you want LVGL, or push raw
  RGB565 frames if you don't. Two ~30-row DMA stripe buffers are plenty; do
  not allocate full-frame DMA buffers.

## 5. Accelerometer (SC7A20, I2C `0x19`)

- Bus: SDA=5, SCL=6, 400 kHz. `WHO_AM_I (0x0F)` should read `0x11`.
- Basic bring-up:
  - `CTRL_REG1 (0x20) = 0x57` → 100 Hz, all axes on.
  - `CTRL_REG4 (0x23) = 0x80` → block-data-update, little-endian, +/-2 g.
  - Poll `STATUS (0x27)` for ZYXDA (bit 3), then read 6 bytes from
    `OUT_X_L (0x28)` with auto-increment (set bit 7 of the register address).
- Conversion: each axis is 12-bit left-justified → `counts = raw >> 4`,
  1 count = 1 mg at +/-2 g.
- The NFC chip shares this bus and can wedge it on droopy battery power: use
  **bounded** I2C timeouts and retry — never wait forever.

## 6. LEDs (6x WS2812B on GPIO3)

- Single data line, GPIO3, GRB byte order, driven by the RMT peripheral
  (`led_strip` driver, 10 MHz RMT resolution). Count = 6.
- Physical order looking at the front:
  `0 UpperLeft, 1 UpperRight, 2 MiddleRight, 3 BottomRight, 4 BottomLeft,
  5 MiddleLeft`.
- Keep brightness modest — 6 LEDs at full white can brown-out the board on
  AA power.

## 7. NFC (MFRC522, I2C `0x26`) — optional

- Same SDA/SCL pins. It is power-hungry: keep it off except while scanning
  (duty-cycle the 13.56 MHz field ~750 ms on/off since the chip has no
  low-power card detect).
- If you don't need NFC, don't init it — your battery life will thank you, and
  the I2C bus stays clean for the accelerometer.
- Suspend accel polling while actively scanning NFC to keep one master-side
  user on the shared bus.

## 8. Radio (BLE 5, NimBLE) — optional

- ESP32-C3 BLE via NimBLE: extended advertising + passive scan is the proven
  pattern. Stock NimBLE buffer tuning OOMs this board's heap — if you enable
  BLE, trim to connectionless sizes (small adv buffers, few ACL/event mbufs,
  1M PHY only, no periodic adv).
- Never tear the stack down and re-init it in one boot (`nimble_port_deinit`
  is a one-way door); init once, then start/stop adv+scan.

## 9. Console / debug helpers worth stealing

- If you write your own firmware, keep at least a button-inject + print path
  over USB-Serial-JTAG — being able to drive buttons and read state over USB
  is what makes HAL development fast.
- Keep chunk sizes under 256 bytes per USB write (USB-Serial-JTAG RX ring).

## 10. Minimal HAL checklist

- [ ] GPIO init + Start-button read (GPIO9, active-low).
- [ ] HC165 bit-bang read returning 8 active-low bits in
      A,B,Home,Down,Left,Right,Up,Aux1 order.
- [ ] 10 ms poll + debounce.
- [ ] SPI ST7789 init (40 MHz, RGB565, invert/swap/mirror) + fill-screen test.
- [ ] I2C init (400 kHz) + accel WHO_AM_I check + mg read.
- [ ] WS2812 RMT init + chase pattern (dim!).
- [ ] NFC/BLE only if your project needs them.

Pin numbers and register values above are the complete source of truth —
no other file is needed.
