# Status

Updated: 2026-11-22

## Goal

Polled I2C slave at 400 kHz with zero data errors; 1-Wire engine stays
interrupt-free. Interrupt path exists as an option (`I2C_USE_IRQ`), polled is
the default.

## Current numbers

| setup | result |
|-------|--------|
| 400 kHz, polled, `repeat 0x00 1` 50 000 | 0 diff, 0 nack |
| 400 kHz, polled, `repeat 0x6A 2` 50 000 | 0 diff, 2 nack |
| 400 kHz, polled, `stress 20000` | 0 invalid, 0 errors, 24 retries (0.12%) |
| 300 kHz, polled, 500 000 reads | 0 diff, 0 OUF |
| worst manager step | 6 us, `MGR_READ` |

## Blocker

24 retries per 20 000 reads at 400 kHz: first byte loses its MSB when a
manager step (6 us) covers the 1.44 us low period. Every manager step must be
<= 1 us.

## Next experiment

Slice `MGR_READ` (manager.c): final `ow_read_poll` completion, `ds18b20_crc_ok`
and the `ow_copy9(sp_sel, ...)` bookkeeping as separate phases. Then measure
`ow_hal_feed` (ow_hal.c), which programs TMR1 + DMA in one call.

Acceptance: `REG_MAX_STEP` <= 1 us, `sweep` 0 diff, `stress 20000` 0 retries.

## Commands

```
make -j8 && make test                    # firmware + 332 host checks
make flash-stlink                        # or tools/bench.ps1 -Flash
tools/bench.ps1 -Flash                   # flash + sweep + one-line summary
tools/bench.ps1 -Cmd "stress 20000"      # flash skipped, custom scenario
```

Tester port COM10, 115200. It runs its own suite on boot: send Ctrl-C twice
and wait for `ready:` before commands.

## Rejected / known

- `memcpy` in manager steps: replaced by `ow_copy8/ow_copy9` (LDRD/STRD).
- Manager steps inside the I2C burst: raised first-byte errors to 2%.
- `DT` preload at STOP or before ADDR clear: ignored by the peripheral.
- Commit must not copy `REG_SEQ` (0x08): it broke the seqlock (fixed).
- T50/T51 inject a fault and force the slow first-byte path; they are test
  harness issues, not traffic.
