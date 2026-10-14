#pragma once

// hal_i2c.h only needs the bus handle type; the emulator has no I2C bus.
typedef struct EmuI2cBus *i2c_master_bus_handle_t;
