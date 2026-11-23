# Status

Updated: 2026-11-22

## Decision

Firmware is back at `e9ba176`. It measured, with the current value-checking
tester, at 400 kHz: 50 000 single-byte reads, 0 diff, 0 nack, 0 OUF. The
IRQ/RAM/precompute/burst work after it was a regression and has been dropped.
Only the test tooling was kept (repeat/diag/sweep, T51 without the MSB mask).

## Current numbers (e9ba176)

| setup | result |
|-------|--------|
| 400 kHz, `repeat 0x00 1` 50 000 | 0 diff, 0 nack, 0 OUF |
| 400 kHz, `stress 20000`, cycle running | 20000 good, 5 retries, all 0xFF tails, FAIL |
| 400 kHz, `stress 20000`, cycle paused (`cmd 4`) | 20000 good, 0 retries, PASS |
| 100 kHz, `stress 20000`, cycle running | 20000 good, 11 retries (6 nack, 5 tails), 517 s, FAIL |
| 100 kHz, 1/2/8-byte blocks, 20 000 each | 0 diff |
| 300 kHz, 1-byte, 20 000 | 0 diff |
| `run full` (400 kHz) | 55/55 passed, 190 s |
| host tests | 332 checks, 0 failures |

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

## Next

1. Single hypothesis: a 1-Wire step starves `i2c_poll()` long enough to miss
   the TX window.  Measure the worst gap between `i2c_poll()` calls
   (instrumentation only, no behaviour change), then feed `i2c_poll()` from
   inside that gap.
2. Only after that: revisit the post-e9ba176 candidates (publish slicing).

## Commands

```
make -j8 && make test
tools/bench.ps1 -Profile stress -Flash
tools/bench.ps1 -Profile full
tools/bench.ps1 -Flash -Cmd "repeat 0x00 1 50000" -Speed 400
```

Tester: COM10, 115200; Ctrl-C twice, wait for `ready:`. Baseline worktree:
`C:\Users\mr.smith\at32f421-1wire-baseline` (detached at e9ba176).

## Rejected (do not repeat)

- Tester `map_crc16()` skipping `REG_DT_FULL`(0x49)..`REG_POLL_GAP_SUB`(0x5F):
  the skip came with the post-e9ba176 forensics registers, but the baseline
  firmware includes those bytes, and a skipped zero still advances the CRC, so
  every map read failed. Fixed 2026-11-22 in `tests.c`; keep tester and firmware
  `crc_skip` sets identical.
- Tester OUF-counter delta as a verdict: reads of `REG_OUF_END` (0x4A) and the
  two-byte `REG_DIAG_ACK`+`REG_DIAG_REASON` transfer are not stable on this
  slave (saw 130 -> 37 and 0x00 -> 0x80 on a zero register).  The stress
  verdict now uses retries only.
- I2C interrupts, SRAM service, first-byte precompute, 64-poll burst,
  publish/read slicing: measured worse than e9ba176.
- `DT` preload at STOP or before ADDR clear: ignored by the peripheral.
- Manager steps inside the I2C burst: first-byte errors rose to 2%.

