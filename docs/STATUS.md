# Status

Updated: 2026-11-24

## Decision

Firmware is `e9ba176` plus three robustness fixes (periodic rescan, strong
pull-up release in the recovery paths, refused `start` no longer published as
a fresh reading).  No timing or protocol change, so the I2C measurements below
still apply.

## Current numbers (e9ba176 + P1 fixes)

| setup | result |
|-------|--------|
| 400 kHz, `repeat 0x00 1` 50 000 | 0 diff, 0 nack, 0 OUF |
| 400 kHz, `stress 20000`, 9-device cable, cycle running | 20000 good, 0-25 retries per run, FAIL |
| 400 kHz, `stress 20000`, one sensor, short wire, external power | 1/0/0 retries, 2 PASS of 3 |
| 400 kHz, `stress 20000`, cycle paused (`cmd 4`) | 20000 good, 0 retries, PASS |
| 400 kHz, `stress 20000`, PA10 floating (no 1-Wire devices) | 3/3 runs, 0 retries, PASS |
| 400 kHz, `stress 20000`, 100 uF on the 3.3 V rail | 7/8/7 retries, FAIL |
| 400 kHz, `stress 20000`, 1-Wire DMA priority lowered | 4/10/4 retries, FAIL |
| 100 kHz, `stress 20000`, cycle running | 20000 good, 11 retries (6 nack, 5 tails), 517 s, FAIL |
| 100 kHz, 1/2/8-byte blocks, 20 000 each | 0 diff |
| 300 kHz, 1-byte, 20 000 | 0 diff |
| `run full` (400 kHz) | 55/55 passed, 190 s |
| host tests | 333 checks, 0 failures |

## Nuance

A single `reg` read taken exactly when the manager runs can still lose the MSB
(seen once: 0xDA); the 50 000-read loop is clean because the pointer write and
read follow each other.  This is the same 1.44 us window, not a new defect.

`stress 20000` is clean on data (0 torn, 0 invalid, 0 errors) but fails the
zero-tolerance verdict on retries, and every retry is a tail of 0xFF starting
at a random offset (64, 120, 149): the slave stops handing over bytes, it is
not a bit flip.  Retries happen only while the 1-Wire cycle runs: with `cmd 4`
(pause) the same run is 20000/20000 with 0 retries at 400 kHz.  At 100 kHz the
count rises to 11 and adds 6 nack on the single-byte reads around the 256-byte
map read, so this is not the bus speed margin.  The manager state at a retry is
`read` or `eeprom`, never `publish`, and the worst manager step stays 10 us.
`run full` T36 soak shows the same signature: 2 retries, both at offset 64.

Measured with temporary instrumentation (since removed), 400 kHz, cycle on:
- worst gap between `i2c_poll()` calls: 11 us against a 22.5 us byte budget;
- long frame stopped before byte 256: 0 occurrences while retries happened;
- run-to-run spread on the same build: 0, 1, 4, 5, 11, 25 retries per 20000
  reads, so a single run proves nothing.

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

