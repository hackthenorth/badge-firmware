#!/usr/bin/env python3
"""Shared I2C bus must never wait forever.

The accelerometer and the NFC reader share one I2C bus. An unbounded wait
(timeout -1, or portMAX_DELAY on a bus lock) in either driver turns one bus
glitch (e.g. the MFRC522 misbehaving when its supply droops on batteries)
into a permanently wedged task holding the IDF bus lock — and NFC setup runs
under the LVGL lock, so the whole UI bricks with idle current and no reboot.
Every wait on this path must be strictly finite; glitches then degrade to
retries. See main/hal/hal_accel.cpp for the incident.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
FILES = [ROOT / 'main/hal/hal_accel.cpp', ROOT / 'main/hal/rc522_new_api.cpp']

failures = []
for path in FILES:
    text = path.read_text()
    for i, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if stripped.startswith('//'):
            continue
        # i2c_master_transmit[_receive](..., timeout): -1 means portMAX_DELAY.
        if re.search(r'i2c_master_transmit\w*\(.*,\s*-1\s*\)', line):
            failures.append(f'{path.name}:{i}: unbounded I2C transfer timeout (-1)')
        if 'portMAX_DELAY' in line:
            failures.append(f'{path.name}:{i}: unbounded wait (portMAX_DELAY)')

# NFC must suspend accel polling while it owns the bus (enable, before first
# I2C use) and resume it on disable; otherwise the cache task's transfers
# contend for the IDF bus lock inside i2c_master_bus_add_device, whose take
# is infinite. Count call sites, not exact placement.
nfc = (ROOT / 'main/hal/hal_nfc.cpp').read_text()
uses = len(re.findall(r'hal_accel_set_suspended\s*\(', nfc))
if uses < 3:
    failures.append(f'hal_nfc.cpp: expected suspend/resume pairing for '
                    f'hal_accel_set_suspended, found {uses} call(s)')

if failures:
    print('I2C bounds: FAIL')
    for f in failures:
        print(' ', f)
    sys.exit(1)
print('I2C bounds: no unbounded waits on the shared I2C path PASS')
