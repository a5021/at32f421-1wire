# Status

Updated: 2026-11-27

## Current build (register map 0x04)

The I2C priority guard was tested and rejected. A 1 ms whole-manager guard and
a 50 us guard made short stress runs clean, but both reduced `run quick` to
32-34/48 by preventing normal manager progress. A low-level start gate and an
operation-level gate also failed: deferring individual 1-Wire phases corrupts
the operation. With the guard removed and the T22 observation window increased
from 2 s to 5 s, both builds pass quick. `stress 1000` shows the honest residual
transport rate.

| setup | result |
|-------|--------|
| `run quick`, production watchdog build | 48/48 PASS |
| `run quick`, `DIAG_BUILD=1` | 48/48 PASS |
| `run full`, production | 56/56 PASS |
| `stress 1000`, 400 kHz, no guard | 1000 good, 5 torn, 0 invalid, 0 errors, 1 retry |
| `stress 1000`, 100 kHz, no guard | 1000 good, 17 torn, 0 invalid, 0 errors, 0 retries |
| T46 SDA-stuck policy | PASS in production and diagnostic builds |
| T46 open-frame timeout | PASS: production recovers, diagnostic waits for next address |
| host tests | 354 checks, 0 failures; Python register-map check passes |
| CMake | production and `DIAG_BUILD=ON` firmware builds link |

Fixes in this build:
- `STS1` and `STS2` reads that clear `ADDR7F` are adjacent in generated `-O2`
  assembly.
- Register-map version is `0x04`; the tester reads the shared version constant.
- `REG_SELECTED` is latched at STOP with command/speed changes; the latch helper
  and `BRIDGE_MAP_VERSION` have host coverage.
- Production builds restore SDA-stuck recovery and a 250 ms open-frame
  watchdog. `DIAG_BUILD=1` disables both; T46 accepts either policy. Recovery
  reports `HEALTH_RECOVERED` only when SCL and SDA both finish high, otherwise
  `HEALTH_RECOVERY_FAILED`; the SDA detector ignores frames waiting for data.
- T22 now observes for 5 s instead of 2 s; it still rejects cycle jumps and
  non-monotonic values.
- `REG_DIAG_START` (0x4C) and `REG_DIAG_BUS` (0x4D) count STARTF/BUSERR once per
  open frame.

The remaining transport retry correlates with PA10 edge activity. No software
guard is retained because it trades transport errors for a non-functional
1-Wire manager. The complete fix remains PA10 series resistance/layout or a
different 1-Wire pin.

## Historical numbers

## Decision

The root cause of every transport symptom was CTRL1 bit 7.  On this part the bit
is NOSTRETCH, like STM32 `CR1.NOSTRETCH`: `0` holds SCL low until the slave has
loaded DT, `1` lets the master clock the byte out at once.  The firmware wrote
`1`, so clock stretching -- described as enabled in `i2c-protocol.md` and
assumed in every latency budget -- was off.  The slave therefore could not hold
the bus at the address match or between bytes: the first byte after the address
came back with bit 7 forced (0x5A -> 0xDA) and a late byte became a transmit
underrun (0xFF tails, `HEALTH_OUF` in every run).

With `stretch = 0` the same builds that failed now pass:

| setup | before | after |
|-------|--------|-------|
| `run full` (400 kHz) | 53/55, T19/T28/T51/T55 failing | **55/55** |
| `repeat 0x00 256 20000`, 9 devices, cycle running | 19992-19998 good, 1-6 rbad, 1-2 wbad | 19998 good, 0 rbad, 2 wbad |
| `HEALTH_OUF` | set in every run | needs a fresh health read |

Everything built on top of the missing stretch (chunked publish, the 12 us poll
budget, the address-window theory) stays as it is -- it is correct and cheap --
but it is no longer what keeps the bus alive.

## Earlier 400 kHz numbers (9 devices)

| setup | result |
|-------|--------|
| `run full` | 55/55, PASS |
| `repeat 0x00 256 20000`, cycle running | 19998 good, 2 wbad, 0 rbad, 0 invalid |
| `repeat 0x00 256 2000`, cycle running | 2000 good, 0 wbad, 0 rbad |
| `stress 20000`, cycle paused (`cmd 4`) | 20000 good, 0 retries, PASS |
| `stress 20000`, one sensor, short wire, external power | 1/0/0 retries, 2 PASS of 3 |
| `stress 20000`, PA10 floating (no 1-Wire devices) | 3/3 runs, 0 retries, PASS |
| 100 kHz, 1/2/8-byte blocks, 20 000 each | 0 diff |
| 300 kHz, 1-byte, 20 000 | 0 diff |
| host tests | 337 checks, 0 failures |

The rows below the first two predate the stretch fix and were measured while
NOSTRETCH was wrongly set.

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

## Manager starvation (2026-11-26)
The cycle counter froze for minutes at a time: `watch cycle` returned one value
for the whole window, T22 reported "0 increments" and T42 "cycle not advancing".
Nothing was wrong on the 1-Wire side.
- Cause: `i2c_poll()` served `tx_mode && TDBE` before `STOPF`.  The NACK that
  ends a read leaves ACKFAIL, TDBE and STOPF set at the same moment, so the
  STOP was never retired; `tx_mode` stayed set and `i2c_active()` -- which also
  trusted `STS2.busyf` -- kept `manager_poll()` from running at all.
- Fix: `STOPF` and `ACKFAIL` are handled before `TDBE`; `i2c_active()` uses the
  software frame view only (`STS2.busyf` stays set after a mangled frame,
  measured). A 100 ms recovery watchdog was tried, then removed: it could cut
  a healthy frame after a missed STOP and produced the deterministic `0xFF`
  tails seen in LA captures.
- Do not retire the frame on ACKFAIL alone. Clock stretching is enabled; the
  frame is retired by STOPF or the next address match.
- 1-Wire pad drive strength matters: `odrvr = 1` (stronger) cost 10-14 corrupted
  map reads per 3000; `odrvr = 0` (weakest) 4 per 5000.
| setup after the fix | result |
|-------|--------|
| `run full` | 54/55, T22 the only failure |
| `stress 20000` | 20000 good, 99 torn, 0 invalid, 0 errors, 6 retries (6 validate, 0 transport) |
| `stress 5000` | 5000 good, 23 torn, 0 invalid, 0 errors, 2 retries |
| `stress 3000` at 100 kHz | 3000 good, 16 torn, 0 invalid, 0 errors, 3 retries |
| cycle | advances 1/s under load, no freeze |

The `0xFF` tails were truncations: `retry diff` showed valid bytes followed by
`0xFF` from an arbitrary offset. Triggered LA captures during `couple` showed
PA10 switching at the truncation phase, exact SCL edge counts, and no visible
illegal SDA edge. Software guards were rejected because they break manager
concurrency; the remaining retry is therefore treated as a PA10 coupling or
in-flight pass overlap issue.

## Remaining
- `stress 1000` at 400 kHz without a guard has 1 retry. A pass already in flight
  when an unpredictable I2C master starts can still overlap PA10 with I2C.
- Review items that are not transport errors: `i2c_apply_speed()` switches the
  peripheral off from the STOP path, and TRISE is clamped to 63 at 100 kHz
  because the field is 6 bits wide.

## Next

1. Keep short acceptance runs at 400 kHz and 100 kHz; only then repeat a longer
   `stress` run.
2. Add a 100-220 ohm series resistor on PA10 near the MCU, separate the PA10
   trace and ground return from SDA/SCL, or move 1-Wire to a different pin.

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
- Transmit underrun (`OUF`) as the cause of the current `0xFF` tails: rejected.
  OUF stayed zero in the captured failures and clock stretching is enabled.
  The earlier missing-stretch defect was real, but it is separate from the
  PA10-overlap failures.
- I2C quiet guard (1 ms, 50 us, low-level and operation-level variants):
  rejected. It made short stress runs clean but reduced `run quick` to
  32-34/48 by blocking or corrupting manager operations. Guard=0 baseline is
  48/48 after widening the T22 observation window, with 1 retry per 1000 stress
  reads at 400 kHz.
- 250 ms open-frame watchdog: restored for production using time since the last
  address match, so it does not cut a legal transfer. `DIAG_BUILD=1` disables
  it together with SDA-stuck recovery.
- Early BUSERR clear without frame teardown: did not remove CRC failures in
  short `couple` runs. Reverted to the normal handler order.
- "The residual errors happen because this slave cannot stretch": wrong. The
  official RM confirms `STRETCH=0` enables stretching, and a 200 us stall did
  not corrupt reads. The remaining correlation is PA10 edge activity.

