# Changelog

## Unreleased

### Fixed
- **Bridge:** CRC-16 is table-driven. The bitwise loop blocked the CPU for
  ~150 us inside `publish()`; while the CPU is busy the master can corrupt the
  byte it is transferring, so the loop is now ~25 us.
- **Tester:** both link speeds are calibrated to the measured SCL. The RP2040
  controller adds a fixed overhead per period, so 100 kHz requested gave
  95 kHz and 400 kHz gave 365 kHz; the tester now requests 105.4 kHz and
  440 kHz, which measure 100.1 and 401 kHz. `sample` reports one decimal.
- **Tester:** single-shot reads in T22/T46/T55 are retried, so the known
  rare corrupt byte cannot turn a pass into a failure.

### Added
- **Bridge:** CRC-16/CCITT (`REG_MAP_CRC`, `0x63`) over the published
  snapshot. I2C has no link CRC, so without it a flipped bit in a
  temperature read as a plausible value. The master zeroes the live and
  control bytes (`0x02`, `0x08`, `0x09..0x0F`, `0x60..0x64`) before checking.
  `REG_VERSION` is now `0x03`.
- **Bridge:** one-shot fault injection through `REG_FAULT` (`0x62`): bit
  flip, dropped byte, extra frame, dropped frame. Frames are counted at the
  address match and the injection is committed at STOP, so it applies to the
  next transaction and never leaks.
- **Tester:** `fault <code>` and seeded `fuzz [ops] [seed]`, plus T48-T54:
  host-side bit-banged address corruption, frame-count ground truth, map-CRC
  detection of a flipped bit, dropped-byte handling, extra-clock tolerance,
  corrupted command/speed/selection writes, 400 mixed operations and a seed
  replay. 55 tests total.
- **Bridge:** bus-health block at `0x09..0x0E`: a 32-bit matched-address
  counter (`REG_FRAMES`), sticky flags (`REG_HEALTH`: BUSERR, ARLOST, OUF,
  SDA stuck, recovered), and a saturating BUSERR counter. The master
  compares its own transaction count with `REG_FRAMES`; the difference is the
  address-phase loss rate, which the slave cannot observe any other way
  because a NACK on its address raises no flag. `CMD_CLEAR_COUNTERS` clears
  the block. The counter only changes at an address match, so it is read
  without wrap logic and without a seqlock.
- **Bridge:** self-recovery. If SDA stays low for 10 ms with no matched
  address, the slave disables I2C, clocks nine times and issues a STOP in
  open-drain mode, then re-initialises the peripheral at the same speed and
  address. A low SCL is never treated as a fault because it is the slave's
  own clock stretch.
- **Tester:** `couple [n]` (1-Wire edge burst in every frame) and
  `stall [n]` (200 us CPU stall at byte 64 of every frame), plus
  manager-state, transport-step and bit-position retry diagnostics.
- **Tester:** `health [clear]` command and tests T43-T47 (frame accounting,
  counter clear, loss-free accounting, stuck detection, no false alarm).
  47 tests total.

### Fixed
- **Engine:** every TMR1 pass now runs in one-cycle mode (OCMEN) and stops
  itself at its update event; CH3 compare is preloaded with `0` so the line
  is released HIGH by hardware (`ow_hal_done()` no longer stops the timer or
  disables channels). Previously a pass only stopped when `ow_hal_done()`
  ran, so a delayed manager left the bus pulled low.
- **Engine:** `ow_hal_strong_pullup(false)` now disables C3EN and stops the
  timer so the line is fully released after parasite power.
- **Clock:** `clock_config()` PLL multiplier field now follows the vendor
  encoding (multiplier−2 up to ×15, multiplier−1 from ×16); the HICK/2
  fallback previously silently produced ×29 (116 MHz) instead of ×30
  (120 MHz). Clock tree is read back from CRM and published as
  `system_core_clock`; TMR1 divisor and I2C prescalers derive from it.
- **I2C fast mode:** speed prescaler computed as `APB1/(3·SCL)` for 400 kHz
  (100 at 120 MHz) instead of the hardcoded 150 (~267 kHz).
- **Bridge:** `MGR_READ` walk bounded to `min(device_count, DEV_MAX)` so
  `roms[]` is never read past its end; unused temperature slots publish
  `0x8000`/`0xFF` and unused ROM slots `0xFF` after a rescan finds fewer
  devices.
- **Bridge:** aborted I2C transfers (ACKFAIL/BUSERR/ARLOST/OUF) now drop
  `pending_cmd`/`pending_speed` and reset transfer state so a command cannot
  fire on another transaction's STOP.
- **DS18B20:** `ds18b20_temp_poll()` returns `OW_PARAM` when no conversion
  has run instead of reporting the previous scratchpad as fresh.
- **Manager:** `manager_poll()` moved from `app/main.c` to `src/manager.c`
  and is now covered by host tests (`tests/test_manager.c`).
- **Manager:** aborted I2C commands (`manager_command()`) are consumed
  exactly once; a command latched while busy is ignored on subsequent polls.

### Changed
- Register map moved to `docs/register-map.md` (source of truth); README
  keeps a short summary. `tools/onewire_demo.py` constants carry a sync note.
- Clock tree queried at runtime via `clock_ahb_hz()` /
  `clock_apb1_hz()` / `clock_apb2_hz()` instead of hardcoded 120 MHz.
- New docs: `docs/register-map.md`, `docs/i2c-protocol.md`,
  `CONTRIBUTING.md`, `HARDWARE-NOTES.md`.

### Added
- **Bridge:** reads of `0x0F`, `0x60`, `0x61` are the only writable
  registers; every other write is ignored while the pointer still advances.
  `REG_SELECTED` is now writable (low nibble) and a selection outside the
  DS18B20 table publishes nine `0xFF` bytes instead of a stale scratchpad.
- **Tester:** `tools/rp2040-tester/` — RP2040/Pico autonomous conformance
  tester for the `0x42` bridge. 42 tests cover the bus at 100/400 kHz,
  repeated START, turnaround, the register map, read-only enforcement,
  snapshot tearing, sensors, rescans, DS2431, 10 000-read stress and fuzzing.
  SCL is sampled at 62.5 MHz by PIO, so the reported frequency is measured,
  not assumed. USB CDC console has a line editor (history, arrows, Home/End,
  Delete, Ctrl-A/E/K/U/W/L), a preloaded command history and diagnostic
  commands (`temp`, `roms`, `scratch`, `eeprom`, `dump`, `reg`, `cmd`,
  `select`, `probe`, `recover`, `stress`, `sample`, `bench`, `watch`, `adc`).
- **Snapshot seqlock:** `REG_SEQ` (`0x08`) is odd while `publish()` updates
  the map and even when stable; the tester reads it before and after every
  map and retries on a change. This removes false failures from publishes
  that overlap a 256-byte transfer.
- **Bus recovery:** the tester clocks nine pulses and sends a STOP after an
  I2C timeout, releasing a slave that holds SDA low. The bridge skips
  manager work while an I2C transaction is active.
- Host test `tests/test_manager.c` for the manager command path
  (332 checks total, 0 failures).

## 1c431b6 — Optimize hot paths

I2C idle early-out, publish copies, LTO, host block read. 5.7 KB text /
2.7 KB BSS (Make), 4.3 KB text / 2.1 KB BSS (CMake). 320 host checks, 0
failures.

## 4dea8e7 — Initial AT32F421 1-Wire driver, I2C bridge and host demo

Non-blocking, interrupt-free TMR1 + DMA1 engine for DS18B20 and DS2431 on
AT32F421F8P7, CH341A-based I2C host demo.
