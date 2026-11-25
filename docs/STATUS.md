# Status

Updated: 2026-11-25

## Decision

Firmware is `e9ba176` plus four robustness fixes (periodic rescan, strong
pull-up release in the recovery paths, refused `start` no longer published as
a fresh reading) and the P3 snapshot fix: the published map moved to a
dedicated buffer that is never written by a build, so a read can no longer
observe a half-committed register file.

## Current numbers (e9ba176 + P1 fixes + P3 snapshot)

| setup | result |
|-------|--------|
| 400 kHz, `repeat 0x00 256 20000`, 9 devices, cycle running | 19992 and 19998 good, 1-6 rbad, 1-2 wbad, 0 invalid |
| 400 kHz, `repeat 0x00 1` 50 000 | 0 diff, 0 nack, 0 OUF |
| 400 kHz, `stress 20000`, cycle paused (`cmd 4`) | 20000 good, 0 retries, PASS |
| 400 kHz, `stress 20000`, one sensor, short wire, external power | 1/0/0 retries, 2 PASS of 3 |
| 400 kHz, `stress 20000`, PA10 floating (no 1-Wire devices) | 3/3 runs, 0 retries, PASS |
| 100 kHz, 1/2/8-byte blocks, 20 000 each | 0 diff |
| 300 kHz, 1-byte, 20 000 | 0 diff |
| `run full` (400 kHz) | 53/55, 190 s; see Remaining |
| host tests | 337 checks, 0 failures |

Before P3 the same 400 kHz row was 146-191 retries per 20 000 reads.

Measured with temporary instrumentation (since removed), 400 kHz, cycle on:
- worst gap between `i2c_poll()` calls: 11-12 us against a 22.5 us byte budget;
- long frame stopped before byte 256: 0 occurrences while retries happened;
- run-to-run spread on the same build: 0, 1, 4, 5, 11, 25 retries per 20000
  reads, so a single run proves nothing.

### P3: what the 0xFF tails actually were

A logic-analyser capture of SDA/SCL at 4 MHz over 529 map reads showed six
frames whose first bytes were real and whose remainder, CRC field included, was
0xFF, while the slave kept ACKing and the temperatures stayed valid.  Four of
the six broke at byte 0x40, and the tester's own soak reported retries "both at
offset 64" -- the boundary of the 64-byte commit chunk.  `publish()` committed
the shadow map into `g_regs` in four chunks spread over four `manager_poll()`
calls and the master read `g_regs` directly, so a read that overlapped a
publish saw the new first chunk over the previous tail.

Fix: three 256-byte buffers.  A publish builds into `map_build` and the swap to
`map_live` is one pointer store; the buffer a running read may still hold is
parked in `map_spare` and reused only two publishes later.  The read path
latches `manager_map()` at the address match, so one transaction always serves
one snapshot, and `REG_SEQ` changes only at the swap.  Host-owned bytes
(`REG_SPEED`, `REG_CMD..REG_FAULT`) are still read from `g_regs`; a selection
change invalidates the scratchpad immediately (`manager_select()`), because a
stale copy of the previous sensor's scratchpad is a lie.

Result: 146-191 retries per 20 000 reads became 1-6, and T21 is 0 torn over 400
snapshot reads.

With PA10 lifted off the board (1-Wire devices gone, cycle still running, map
frozen with `TEMP_ERROR`) three stress runs in a row were 20000/20000 with 0
retries.  The remaining faults therefore need the 1-Wire bus attached: they
are electrical or bus-load driven, not I2C-slave CPU timing.

One sensor on a short wire with external power, 400 kHz, three runs:
1 / 0 / 0 retries (one nack during `wait`, no 1-Wire work in flight).  The
strong pull-up is still engaged every cycle -- `ds18b20_init(&sensor, true)`
in `manager.c` hardcodes parasite mode, so PA10 drives the line high for the
whole 750 ms conversion -- and it produced no retries, so the pull-up current
is not the coupling.  The difference between 0-1 and 4-25 retries is the
9-device cable.  A residual address-phase nack remains at roughly 1e-5 even
on the short wire; zero tolerance at 400 kHz is probably not reachable.

## Remaining

- `run full` 53/55.  T28 and T55 fail identically on `ed3796c` and on the
  review build, so they are pre-existing: both depend on the selected
  scratchpad being refreshed within two cycles, and T55 asks for a sensor index
  that is not a DS18B20.  T48 flaked once ("missed 0, wrong-address acks 0") and
  passed on the next run.
- Single-shot tests still catch the address window: `T19 speed readback 129`
  (0x01|0x80) and `T51 first byte 83` (0x03|0x80) appear in some runs and pass
  in others.  That is the 1.44 us ADDR window, not a data-path error: T35's full
  map CRC is intact in every run (invalid 0, errors 0).
- Residual transport errors: about 1 write and 1 read per 20 000, both on the
  9-device cable.

## Next

1. Keep the one-sensor setup as the reference configuration: it is the first
   configuration that passes the stress verdict.
2. If the 9-device cable is still wanted: add series resistors at the far end
   and shorten the I2C run, then repeat the 3-run comparison.
3. Decide the acceptance budget: 0 delivered errors with <=1 retry per 20 000
   reads, or a strict zero.  Current data argues the strict zero is not a
   property of this slave.

Tried and useless: 100 uF on the 3.3 V rail (7/8/7 retries) and lowering the
1-Wire DMA priority to low (4/10/4, inside the run-to-run spread).  Both were
reverted; firmware is at e9ba176 again.

### Review fixes (2026-11-25)

- `i2c_bus_recover()` wrote the levels into `GPIOA->omode`, which is the
  output-mode field, and set `cfgr = 0x66`, which leaves PA0 in alternate
  function: the nine recovery clocks never reached SCL.  Now `cfgr` selects
  GPIO output for both pins (`0x05`), the levels go through `ODT`, SCL is read
  back after every release, and the STOP is sent only when all nine clocks
  actually went high.
- A poll after a finished operation returned `OW_OK` regardless of the result,
  so a `OW_NO_PRESENCE` turned into success one call later, and
  `ds18b20_temp_poll()` kept `stage = 1` and re-ran the read.  Each driver now
  stores the terminal status and the DONE phase returns it; the composite poll
  clears its stage on any non-busy result.  Tests assert the sticky failure.
- A failed periodic rescan used to zero the counts first and publish them over
  the previous ROM map.  The search now stages into `roms_next` and copies into
  `roms[]` only on `OW_END`, so a failed pass leaves the last good list.
- `clock_config()` waits for PLL lock and the clock switch are bounded and fall
  back to HICK; `ow_hal_wait()` saturates at 16.7 s instead of letting
  `us + 65534` wrap; `ow_reset_start()` documents that the caller must check
  `ow_hal_busy()` first.
- T35 counted a publish that landed between its two `REG_SEQ` probes as a torn
  read.  After P3 a frame comes from one snapshot (the swap happens at the
  address match), so the probes disagree exactly when the publisher ran in the
  gap: measured 310 crossings of 10 000 reads, matching the ~5 publishes/s the
  cycle runs at while the link is busy; invalid and errors stayed 0.  The
  tester now validates the map in that case instead of failing on the sequence
  alone.

## Commands

```
make -j8 && make test
tools/bench.ps1 -Profile stress -Flash
tools/bench.ps1 -Profile full
tools/bench.ps1 -Flash -Cmd "repeat 0x00 1 50000" -Speed 400
```

Tester: COM10, 115200; Ctrl-C twice, wait for `ready:`. `-Tester` resolves the
`RPI-RP2` volume by label; the drive letter changes and must not be hardcoded.
Baseline worktree: `C:\Users\mr.smith\at32f421-1wire-baseline` (detached at
e9ba176).

## Rejected (do not repeat)

- Tester `map_crc16()` skipping `REG_DT_FULL`(0x49)..`REG_POLL_GAP_SUB`(0x5F):
  the skip came with the post-e9ba176 forensics registers, but the baseline
  firmware includes those bytes, and a skipped zero still advances the CRC, so
  every map read failed. Fixed 2026-11-22 in `tests.c`; keep tester and firmware
  `crc_skip` sets identical.
- CPU starvation of `i2c_poll()` during 1-Wire work: worst measured gap was
  11 us against a 22.5 us byte budget at 400 kHz, with retries present.
- A premature STOP cutting a long read: a long-frame early-stop counter stayed
  0 across runs that did retry.
- Tester OUF-counter delta as a verdict: reads of `REG_OUF_END` (0x4A) and the
  two-byte `REG_DIAG_ACK`+`REG_DIAG_REASON` transfer are not stable on this
  slave (saw 130 -> 37 and 0x00 -> 0x80 on a zero register).  The stress
  verdict now uses retries only.
- I2C interrupts, SRAM service, first-byte precompute, 64-poll burst,
  publish/read slicing: measured worse than e9ba176.
- `DT` preload at STOP or before ADDR clear: ignored by the peripheral.
- Manager steps inside the I2C burst: first-byte errors rose to 2%.
- Transmit underrun (`OUF`) as the cause of the 0xFF tails: with the counter
  added to `0x6C/0x6D`, the worst interval between `i2c_poll()` calls was 12 us
  against a 22.5 us byte budget, and the capture of six truncated frames showed
  no SCL stretching at all, so the slave was never late.  The tails were the
  chunked commit into the live map (see P3 above).

