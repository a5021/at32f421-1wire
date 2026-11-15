# AT32F421F8P7 — standalone 1-Wire (DS18B20 / DS2431)

Non-blocking, interrupt-free 1-Wire driver built on TMR1 + DMA1 for the
AT32F421F8P7 (Cortex-M4, 64K flash, 16K SRAM, TSSOP20).

## Hardware

| Signal | Pin | Notes |
|--------|-----|-------|
| 1-Wire bus | PA10 | TMR1_CH3, MUX 2, open-drain + 4.7k pull-up |
| Strong pull-up | PA10 | push-pull output of the same pin (no MOSFET) |
| I2C2 SCL | PA0 | MUX 4, open-drain, external pull-up |
| I2C2 SDA | PA1 | MUX 4, open-drain, external pull-up |

Parasite-powered DS18B20 is supported by switching PA10 to push-pull while
the conversion runs.

## Engine

* TMR1 runs at 1 MHz (`120 MHz / 120`, `div = 119`).
* SCLK is 120 MHz from the 8 MHz HEXT (PLL ×15). If no crystal is fitted,
  `clock_config()` falls back to HICK/2 (4 MHz, PLL ×30); the timer tick stays
  1 µs either way.
* CH3 = output (PWM mode B), CH2 compare feeds the next low phase through
  DMA1 channel 3.
* CH4 captures the counter at each rising edge into DMA1 channel 4.
* One-cycle mode: a pass stops itself at its update event. CH3's compare is
  preloaded with 0 for that event, so the line is released HIGH by hardware
  (multi-slot writes get that 0 from the DMA feed instead).
* No interrupts: every `_start` arms one hardware pass, `_poll` waits for the
  timer update flag.

Timings live in `src/ow_config.h` (`OW_T_LOW1/LOW0/GUARD`, reset 480/960 µs,
sample threshold `OW_T_SAMPLE_MAX`, capture filter `OW_C4DF`).

## API

All sensor functions are split into `_start` (arms the operation, returns
immediately) and `_poll` (advances the state machine). Buffers passed to
`_start` must stay valid until the matching `_poll` leaves `OW_BUSY`.

```c
#include "clock.h"
#include "ow_hal.h"
#include "ds18b20.h"

static ds18b20_t sensor;

clock_config();
ow_hal_init();
ds18b20_init(&sensor, true);       /* parasite power */
ds18b20_temp_start(&sensor, 0);    /* 0 = Skip ROM */

for (;;) {
    int16_t raw;
    if (ds18b20_temp_poll(&sensor, &raw) != OW_BUSY) {
        /* raw / 16 = °C, restart */
        ds18b20_temp_start(&sensor, 0);
    }
}
```

`onewire.h` exposes reset/write/read primitives and the ROM search state
machine. `ds2431.h` supports Read Memory (no CRC) and block-aligned 8-byte
writes (`Write Scratchpad` → verify echo/PF/CRC-16 → `Copy Scratchpad` →
status `0xAA`, `tPROG` = 11 ms).

## Host bridge (I2C2 + CH341A)

The board exposes the sensor snapshot as an I2C slave at address `0x42`.
I2C2 is polled (no interrupts, clock stretching enabled); the 1-Wire manager
runs in the same superloop and never blocks.

Register map, read linearly from `0x00`:

```
0x00 magic 0x5A
0x01 version
0x02 flags: search done/error, busy, snapshot valid, eeprom ok, temp error
0x03 device count
0x04 DS18B20 count
0x05 last error
0x06 cycle counter, little endian
0x0F speed: 0 = 100 kHz, 1 = 400 kHz
0x10..0x2F 16 x int16 raw temperature (1/16 C, 0x8000 invalid)
0x30..0x3F 16 x sensor status
0x40..0x47 DS2431 bytes
0x48 DS2431 status
0x60 command: 1 = rescan, 2 = reread DS2431, 3 = clear counters
0x61 selected sensor for scratchpad
0x70..0x78 selected DS18B20 scratchpad
0x80..0xFF ROM table (16 x 8 bytes)
```

Run the demo:

```sh
python tools/onewire_demo.py
python tools/onewire_demo.py --loop 3 --scratch --raw
python tools/onewire_demo.py --rescan --speed 400
```

The CH341A bridge itself generates about 100 kHz; `--speed 400` changes the
slave timing, but a 400 kHz master is needed to verify 400 kHz on the wire.

## Build

Requires `arm-none-eabi-gcc` (newlib) and GNU make.

```sh
make            # build/at32f421-1wire.elf + .hex + .bin
make test       # host tests (9 suites, 320 checks)
make size
```

CMake (same toolchain flags, `cmake/arm-none-eabi.cmake`):

```sh
cmake -S . -B build -G "MinGW Makefiles" \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
```

## Flashing and debugging

The board can be wired to either the WCH-Link CMSIS-DAP probe or an ST-Link.
pyOCD talks to the WCH-Link through libusb; the AT32F421F8P7 target and its 64K
flash algorithm come from the Artery DFP pack stored in `tools/packs/`.

Tooling:

* `pyocd` (Python 3.14 user install) + `pyusb` + `libusb-package`
* `tools/packs/ArteryTek.AT32F421_DFP.2.2.0.pack` (included)
* Artery OpenOCD for ST-Link (`make flash-stlink`) is **not** included: it is
  an 8.6 MB GPL binary. Download
  `tool-openocd-at32-0.1100.220911.tar.gz` from
  <https://github.com/ArteryTek/openocd/releases/tag/v1.0.0> and unpack
  `bin-windows_amd64/openocd.exe` plus `scripts/` into `tools/openocd-at32/`.
  Its CMSIS-DAP backend is HID-only and cannot see the WCH-Link v2 interface.

```sh
make flash       # erase/program/verify build/at32f421-1wire.elf
make flash-stlink # same, through Artery OpenOCD + ST-Link
make erase       # full chip erase
make gdbserver   # pyOCD GDB server on :3333
make debug       # arm-none-eabi-gdb + reset halt
```

Probe serial and target are Make variables:

```sh
make flash PROBE=<serial> PYOCD_TARGET=-at32f421f8p7
```

The leading dash in `-at32f421f8p7` comes from the vendor PDSC device name;
keep it. Verified on hardware: SWD at 1 MHz, flash program/verify, reset, and
GDB attach all work. The 8 MHz HEXT crystal is present on the current board
(the clock path selects HEXT and reports 120 MHz).

## License

Project code is MIT licensed; see `LICENSE`. Vendor CMSIS headers, startup
files, linker scripts and the Artery DFP pack keep their own licenses.
