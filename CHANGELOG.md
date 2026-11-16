# Changelog

## Unreleased

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
- Host test `tests/test_manager.c` for the manager command path
  (332 checks total, 0 failures).


I2C idle early-out, publish copies, LTO, host block read. 5.7 KB text /
2.7 KB BSS (Make), 4.3 KB text / 2.1 KB BSS (CMake). 320 host checks, 0
failures.

## 4dea8e7 — Initial AT32F421 1-Wire driver, I2C bridge and host demo

Non-blocking, interrupt-free TMR1 + DMA1 engine for DS18B20 and DS2431 on
AT32F421F8P7, CH341A-based I2C host demo.
