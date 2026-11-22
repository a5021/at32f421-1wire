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

## Next

1. Re-run `stress 20000` and `run full` on this baseline with the new tester;
   record the numbers here.
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

- I2C interrupts, SRAM service, first-byte precompute, 64-poll burst,
  publish/read slicing: measured worse than e9ba176.
- `DT` preload at STOP or before ADDR clear: ignored by the peripheral.
- Manager steps inside the I2C burst: first-byte errors rose to 2%.
