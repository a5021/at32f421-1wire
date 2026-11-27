# Changelog

## Unreleased

### Changed
- **Bridge:** register-map version is now `0x05`. Early NACK count/position
  diagnostics exclude normal final and short-read NACKs, and stress output
  includes GATE/START/BUS/early-NACK counters. `STRETCH` is programmed before
  `I2CEN`.
- **Bridge:** `REG_SELECTED` is latched at
  STOP with command/speed changes, so an aborted write cannot change the active
  scratchpad selection.
- **Bridge:** production builds restore SDA-stuck recovery and a 250 ms
  open-frame watchdog. `DIAG_BUILD=1` disables both for diagnostics. Recovery
  now requires both SCL and SDA high and reports `HEALTH_RECOVERY_FAILED`
  otherwise; the SDA detector ignores frames waiting for data.
- **Bridge:** speed changes are latched at STOP and applied from an idle
  main-loop window, avoiding I2C reinitialisation races with the next START.
  The 100 kHz TRISE clamp is documented as a safe field-width limit.
- **Manager:** `map_copy_chunk()` uses per-word `memcpy` instead of aliased
  `uint32_t *` casts. ARM output remains a word-copy loop; phase-0 max measured
  4 us versus 6 us before.
- **1-Wire:** default 165 us slot is documented as an intentional deviation
  from the DS18B20 120 us maximum. `-DOW_T_GUARD=55` gives a strict 120 us slot;
  it passed full but raised I2C stress retries, so it is not the default.
- **Tests:** HAL request tests cover reset/read/write/wait slot counts and the
  DMA pulse terminator. STOPF trace verification covered 500 writes plus 500
  reads with correct STOP/NACK placement.
- **Bridge:** I2C quiet-guard experiments were removed. They made short stress
  runs clean but broke manager concurrency (`run quick` 32-34/48); the no-guard
  baseline is 48/48 after widening T22 to a 5 s observation window.
- **Tester:** T22 observes cycle progress for 5 s instead of 2 s; monotonicity
  and jump checks remain unchanged.
- **Bridge:** the polled path is the default again (`I2C_USE_IRQ=0`). The main
  loop serves the bus in a burst of 64 polls before any housekeeping, and the
  address handler clears `ADDR` and writes the precomputed first byte before
  the ACKFAIL bookkeeping. Measured: with the manager paused, 400 kHz shows
  0-6 value differences in 20 000 reads; with the manager running, 1 per
  200 000 single-byte reads, but a 10 us manager step still causes 60 underruns
  in `stress 20000`.
- **Bridge:** publish chunks are 16/8/8 bytes (copy/CRC/commit) instead of 64/32/64: the worst manager step fell from 10 us to 7 us and the bottleneck moved to the sensor read poll (`MGR_READ`, subphase 6), which is the next slice. `stress 20000` at 400 kHz still reports underruns, so the polled 400 kHz path is not closed yet.'n- **Bridge:** the I2C byte service can run from interrupt or poll
  (`I2C_USE_IRQ`), the state machine is shared.
- **Bridge:** the byte service (`i2c_poll`, `reg_byte`, `i2c_send_byte` and the
  ISR wrapper) is copied to SRAM at boot (`.ramfunc`): flash wait states alone
  cost more than the master's 1.44 us low period at 400 kHz. The functions are
  `noinline, used` and the linker keeps the section with `KEEP`, so LTO stays
  enabled and still inlines the callees into the RAM code.
- **Bridge:** the first byte of a read is computed when the register pointer
  arrives and written immediately after `ADDR` is retired. Together with the
  interrupt service this removed the MSB loss that made every short read start
  with `0xDA` instead of `0x5A`.
- **Bridge:** a speed change is applied from the main loop when the bus is
  idle; re-initialising the peripheral inside the STOP handler could race the
  master's next address.
- **Tester:** `repeat <reg> <len> <n>` reads one block in a tight loop and
  reports value differences (reference taken after a warm-up), NACKs and the
  underrun counters; `diag` reads the forensics one register per transaction so
  the measurement cannot corrupt itself.

### Fixed
- **Build:** CMake now includes `src/manager.c`; a clean out-of-tree firmware
  build links successfully.
- **1-Wire search:** loss of presence after at least one valid ROM returns
  `OW_NO_PRESENCE`, not `OW_END`. A failed rescan keeps the last good ROM list
  and publishes `FLAG_SEARCH_ERROR`; host regressions cover both search and
  manager behavior.
- **Tester:** register and flag constants now come from `src/manager.h` instead
  of a duplicate header. `tools/check_regmap.py` checks the Python demo subset
  during `make test`.
- **Tester:** T51 no longer masks the MSB of the first byte. A masked compare
  hid exactly the defect this work was about.

### Notes
- Measured with the interrupt path: 300 kHz, 500 000 reads (1-, 2- and 8-byte
  blocks), 0 value differences, 0 underruns, 4 address NACKs; 400 kHz,
  400 000 reads, 0 underruns in `stress`/`soak`, but a residual first-byte
  error of about 1.5e-4 remains because the service path (~1.2 us) is close to
  the 1.44 us low period. The map CRC covers it; 300 kHz is the speed that
  measured clean.

### Fixed
- **Bridge:** an expanded command packet terminates its slot buffer. The DMA
  walks slots `1..N`, so slot `N` is read even though only `N-1` pulses were
  built; the chunked builder left stale data there, which put one extra bit
  slot after every DS18B20 and DS2431 command and shifted the reply.
- **Bridge:** the first byte of a read is written to `DT` before the address
  handler does any bookkeeping or fault injection, because the master starts
  shifting it about one SCL bit time after the match.
- **Bridge:** the transfer direction is sampled after a pending STOP is
  retired. `STS2.DIRF` used to be read before `i2c_stop_done()`, so a read that
  followed a register-pointer write could be decoded with the previous frame's
  direction: the slave ACKed its address, never loaded `DT`, and the master
  read a whole frame of `0xFF` while the CRC check failed. `stress 50000` at
  400 kHz now reports 0 torn, 0 invalid and 0 retries.
- **Bridge:** the SDA-stuck detector now requires SCL to stay idle-high for the
  whole window. A master that is clocking holds SDA low for many bit times,
  which used to look like a 10 ms stall and could start `i2c_bus_recover()` in
  the middle of a transfer.

### Changed
- **Bridge:** the DS2431 read command and the DS18B20 Convert/Read packets are
  expanded into timer slots two bytes per call instead of one ten-byte packet.
  Expanding ten bytes cost 13-14 us of CPU inside a single step; the step is
  now 1-5 us.
- **Bridge:** `ow_crc16` is table-driven like `ow_crc8`. The twelve-byte append
  CRC blocked the CPU for ~4 us.
- **Bridge:** diagnostics carry the measured worst synchronous manager step in
  microseconds (`0x79..0x7B`), the worst `ds2431_read_start` and
  `ds2431_read_poll` (`0x7E..0x7F`), the SDA-stuck recovery count (`0x6D`), the
  underrun count (`0x6F`) and the transmit position at the last underrun
  (`0x7C`/`0x7D`).
- **Tester:** the failure snapshot also prints the slave frame counter next to
  the host transaction counter, and labels `0x7C`/`0x7D` as `ouf <pos>@<ptr>`.
- **Tester:** T51 masks the MSB of the first byte. That bit is lost when the
  slave is late at the address match (it does not drive bit 7 and the pull-up
  reads 1), which is a property of the polled slave, not of the skip
  injection the test is checking.

### Changed
- **Bridge:** a STOP that has not been serviced yet is finished at the next
  address match, so a command can never be applied to the wrong transaction.
  The master no longer needs a gap between the pointer write and the read:
  removing it took torn reads at 400 kHz from 10 per 20 000 to 0.
- **Bridge:** `publish()` no longer rebuilds the map in one blocking call. It
  fills a 256-byte shadow buffer, computes the CRC in 32-byte steps and only
  then copies the shadow into `g_regs`. The host-writable `REG_CMD`,
  `REG_SELECTED` and `REG_SPEED` are preserved across the copy, and a
  selection change during the build restarts it instead of publishing a
  mismatched scratchpad.
- **Bridge:** `REG_STALL`, `REG_STALL_STATE`, `0x68` and `0x69..0x6B` expose
  the worst and average publish-phase durations in microseconds; `CMD=7`
  clears them. Measured at 400 kHz with the word copy: fill 17 us, CRC chunk
  4-5 us, commit 14 us (was 142 us with newlib `memcpy`).

### Fixed
- **Bridge:** the map copies use a word loop instead of `memcpy`. Newlib's
  `memcpy` took ~142 us for 256 bytes (66 cycles/byte), which alone exceeded
  the slave's service-latency budget; the word copy takes ~14 us. The CRC
  loop also uses a precomputed skip table instead of a per-byte predicate.
- **Tester:** T55 reads the cycle counter after the corrupted write, so a
  publish already in flight cannot be mistaken for the post-write one.

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

