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
| 100 kHz, 1/2/8-byte blocks, 20 000 each | 0 diff |
| 300 kHz, 1-byte, 20 000 | 0 diff |
| host tests | 332 checks, 0 failures |

## Nuance

A single `reg` read taken exactly when the manager runs can still lose the MSB
(seen once: 0xDA); the 50 000-read loop is clean because the pointer write and
read follow each other.  This is the same 1.44 us window, not a new defect.

## Next

1. Blocked: `stress 20000` and `run full` cannot produce numbers while the
   tester CRC coverage differs from the firmware (see Rejected). Align
   `tests.c` `map_crc16()` with the firmware `crc_skip()`, then re-run both.
2. Only then decide whether anything from the post-e9ba176 work is worth
   re-applying (candidates: the publish slicing, nothing else).

## Commands

```
make -j8 && make test
tools/bench.ps1 -Flash -Cmd "repeat 0x00 1 50000" -Speed 400
tools/bench.ps1 -Cmd "stress 20000" -Speed 400
```

Tester: COM10, 115200; Ctrl-C twice, wait for `ready:`. Baseline worktree:
`C:\Users\mr.smith\at32f421-1wire-baseline` (detached at e9ba176).

## Rejected (do not repeat)

- Tester `map_crc16()` skips `REG_DT_FULL`(0x49)..`REG_POLL_GAP_SUB`(0x5F) but
  the e9ba176 firmware `crc_skip()` includes those bytes. They read as 0x00,
  yet a skipped zero still advances the CRC, so every map read reports
  `map crc XXXX != YYYY`. Measured 2026-11-22 at 400 kHz on e9ba176: `stress 10`
  0 good / 10 invalid (0 timeout, 0 nack), `run quick` 38/47 with all nine
  failures CRC or CRC-derived (`T04 T15 T21 T25 T51 T52 T53 T54 T55`), while
  `repeat 0x00 1 10` was 10/10 clean and the dumped map's stored CRC 0x9E28
  matched the firmware coverage exactly. `stress 20000` prints only at the end
  and each failing iteration costs about 2 s, so it did not finish in 900 s.
  Do not run stress/full on this pairing; fix the tester coverage first.
- I2C interrupts, SRAM service, first-byte precompute, 64-poll burst,
  publish/read slicing: measured worse than e9ba176.
- `DT` preload at STOP or before ADDR clear: ignored by the peripheral.
- Manager steps inside the I2C burst: first-byte errors rose to 2%.

