---
title: "2026 Hacker Badge — Factory Flash & Functional Test Procedure"
subtitle: "Production test instructions for assembly / fabrication"
date: "Revision A"
geometry: margin=2cm
colorlinks: true
linkcolor: black
urlcolor: blue
---

# 1. Purpose & Scope

This document describes how to **flash firmware** onto each assembled 2026 Hacker
Badge and how to **functionally test** every board before it ships. The whole test
is driven from a web browser and the badge's own built-in factory test — no
special programmer, jig, or command-line tools are required.

**Target board:** 2026 Hacker Badge
**Processor:** Espressif ESP32-C3 (RISC-V, USB-Serial-JTAG built in)
**Connection:** USB-C cable, direct to a computer (no external adapter needed)

**Quantity for this run: 50 boards.** Every board must pass.
Estimated time once the operator is trained: **about 30 seconds per board.**

---

# 2. Equipment Required (per test station)

| Item | Notes |
|------|-------|
| Computer | Windows, macOS, or Linux |
| Web browser | **Desktop Google Chrome, Microsoft Edge, or Opera.** Firefox and Safari are **not** supported. |
| USB-C cable | Data-capable (not charge-only). |
| Internet **or** local copy of the flashing page | See Section 3. |
| 2 × AA batteries | One set only — **reuse the same pair for every board** (Section 6). |
| NFC card / tag | **Optional.** Any 13.56 MHz card (transit card, hotel key, blank tag). Only needed if you want to test a card scan; see Section 5, Screen 3. |

---

# 3. One-Time Station Setup

1. Open the flashing web page in Chrome/Edge/Opera:

   **`https://hackathon.github.io/badge-firmware/`**

   *(If the station has no reliable internet, the page can also be run from a
   local copy — request the offline package and open `index.html` through a
   local web server. The page must be served over **HTTPS or localhost**;
   opening the file directly will not allow USB access.)*

2. Confirm the page loads and shows the **"Flash a Badge"** button. If instead it
   shows a red "this browser can't access serial devices" message, you are in the
   wrong browser — switch to Chrome, Edge, or Opera.

---

# 4. Flashing Procedure (per board)

> Perform this for **every** board.

1. **Connect** the badge to the computer with the USB-C cable.
2. Click **"Flash a Badge"** on the web page.
3. A small window lists serial ports. Select the badge's port
   (typically shown as *"USB JTAG/serial debug unit"* or similar) and click
   **Connect**.
4. Flashing begins **automatically** — do not touch the cable. The progress bar
   fills and the status shows *Connecting → Loading firmware → Flashing → Restarting*.
5. Wait for the green **"Flashed successfully"** success screen.
6. The badge **automatically reboots into the built-in Factory Test** (Section 5).
   The first screen is the **Buttons** test. **If it does not reboot on its own
   within a few seconds, unplug the USB-C cable and plug it back in.**

**If flashing fails** (red error screen): unplug, re-seat the cable, and click
**"Try Again."** If it still fails, try a different USB-C cable and a different
USB port — most failures are cable or port problems.

---

# 5. Functional Test Procedure (per board)

After flashing, the badge boots directly into the **Factory Test**. The operator
interacts with the **badge's own buttons and screen** — not the computer. The test
is bilingual (English + Chinese) on screen.

At the top of every screen is a colored status strip with three cells —
**BTN, LED, NFC** — that turn **green** as each stage passes. The current stage has
a cyan outline.

### Screen 1 — Buttons

- Title shows **"Buttons"** / *按下所有按钮* ("Press every button").
- A pad is shown on screen for each button. **Press each of these 8 buttons once:**
  **Up, Down, Left, Right, A, B, Home, Start.**
- Each pad turns **green** when its button registers. When all 8 are green the
  test **advances on its own** to the LED screen.
- A button that never turns its pad green, or a pad that turns green without being
  pressed (stuck button), means this board fails the button test.

### Screen 2 — LEDs

- Title shows **"LEDs"**. On-screen prompt: **"A = working   B = broken"** /
  *灯光正常按A故障按B*.
- The LED strip cycles **Red → Green → Blue → White** (the current color name is
  shown on screen). Let it run through at least one full cycle.
- Watch all **6 LEDs**. If every LED lights and shows the correct colors together,
  press **A** (working). If any LED is dark or shows a wrong color, press **B**
  (broken).

### Screen 3 — NFC (optional card scan)

- Title shows **"NFC"**. On-screen prompt: **"Scan a card, or A to skip"** /
  *扫描任意卡片或按A跳过*.
- **If you have an NFC card on hand:** hold it flat against the back of the badge.
  When it reads, the test advances automatically and NFC is marked passed.
- **If you do not have a card:** press **A** to skip. This is fine — the **NFC
  reader chip is still verified automatically** in the background (reported as
  "NFC chip" on the Summary). Only the optional card-read is skipped.

### Screen 4 — Summary

The Summary screen lists every check with a status mark:

- **Green check** = passed
- **Red X** = failed
- **Grey dash** = skipped / not run (e.g. NFC card scan when skipped)

The five lines are: **Buttons, LEDs, NFC, Accel, NFC chip.**

**A board PASSES when these four show a green check:**
**Buttons, LEDs, Accel, and NFC chip.**
The **NFC** line may be a green check (card scanned) **or** a grey dash (card
skipped) — both are acceptable.

Press **A** on the Summary to run the test again if you need to recheck anything.

---

# 6. Battery Test (2 × AA)

This confirms the board runs on battery power, not just USB. **Use one set of AA
batteries and reuse it for every board.**

1. After the board passes the USB functional test, **unplug the USB-C cable.**
2. Insert the **2 × AA batteries.** Match the flat **(−)** end and the bump **(+)**
   end of each battery to the **−** and **+** marks printed in the battery holder.
3. **Turn ON the power switch** at the top edge of the badge.
4. Confirm the **screen powers on** and the badge starts the factory test on
   battery power alone.
5. **Turn OFF the power switch, remove the batteries,** and keep them for the next
   board.

**PASS:** the screen lights up and runs on battery power.
**FAIL:** screen stays dark, flickers, or will not start on batteries.

---

# 7. Important Notes

- **Do not write on, label, sticker, or otherwise mark the badges.** Keep passed
  and failed boards in separate trays/areas instead. The badges ship as-is and any
  marking is visible to the end user.
- **Reuse the same AA batteries** for the battery test on every board — they are
  only inserted briefly to confirm power.
- A board only needs to be flashed once. If a board has already passed and you
  re-flash it, that is harmless.
