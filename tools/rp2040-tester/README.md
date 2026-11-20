# RP2040 I2C conformance tester for the AT32F421 1-Wire bridge

Autonomous tester for the bridge at `0x42`. It runs a fixed suite at boot,
prints one line per test over USB CDC and sets the Pico LED to the verdict.
No host software is required; the USB port is only the log.

## Wiring

| Pico | Bridge | Notes |
|------|--------|-------|
| GP4 | PA1 (SDA) | I2C0 SDA |
| GP5 | PA0 (SCL) | I2C0 SCL, also the PIO sampler input |
| GND | GND | common ground |
| 3V3 | - | 2.2 kOhm pull-up on SDA and SCL |

- Power the AT32 board as usual; do not back-power it from the Pico.
- **Disconnect the CH341A.** Its weak pull-ups and cable capacitance prevent
  a clean 400 kHz bus.
- 2.2 kOhm is the starting point. With long wires or high capacitance, drop
  to 1.5 kOhm; a 4.7 kOhm pull-up cannot meet the 300 ns fast-mode rise time.

## Build

Requires the Pico SDK 2.1.x and `arm-none-eabi-gcc`.

```sh
export PICO_SDK_PATH=/path/to/pico-sdk      # PowerShell: $env:PICO_SDK_PATH=...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The PIO sampler is a single hand-encoded `in pins, 1` instruction, so the
build does not need the `pioasm` host tool.

Flash: hold BOOTSEL, plug the Pico in, copy `build/i2c_tester.uf2` to the
`RPI-RP2` drive.

## Running

On power-up the tester waits up to 3 s for the USB console, probes `0x42`,
and runs the full suite. Commands:

```
run quick        T01-T34, about 40 s
run full         T01-T42, about 3 min
run soak N       full suite with the T36 soak extended to N seconds
results          print the last results again
abort            stop after the current test (also Ctrl-C or 'a' during a run)
info             tester version, SDK, clocks, pins, sampler rate
status           uptime, link speed, target, last run, snapshot summary
ping             probe 0x42 only
scan             probe every address 0x08..0x77
speed 100|400    set the link and the slave timing, safely
rescan           queue a 1-Wire ROM search
bootsel          reboot the Pico into the UF2 bootloader
? or help        command list
```

The prompt is `> `. The firmware echoes typed characters and supports
backspace, so the terminal needs no local echo. While a run is active only
Ctrl-C and `a` are read: they print `[abort requested]` and stop after the
current test.

LED:

| Pattern | Meaning |
|---------|---------|
| fast blink | suite running |
| solid | all tests passed |
| N blinks, pause | first failing suite: 1 bus, 2 map, 3 sensors, 4 stress |
| off | idle or no target |

## Tests

### Bus and transport (T01-T11)

| ID | Check |
|----|-------|
| T01 | ACK at `0x42` |
| T02 | NACK at `0x50-0x52`, `0x7F` |
| T03 | 16-byte header, magic, version |
| T04 | full 256-byte map, validation |
| T05 | register pointer auto-increment |
| T06 | pointer wraps `0xFE -> 0xFF -> 0x00` |
| T07 | repeated START read, then a normal transaction |
| T08 | pointer-to-read gap 0, 2, 5, 10, 20, 50 us |
| T09 | 400 kHz SCL: 390-410 kHz, duty sane |
| T10 | maximum SCL low (stretch) below 5 ms |
| T11 | 100 kHz vs 400 kHz duration ratio 3.0-5.0 |

SCL is sampled by PIO at 62.5 MHz (16 ns resolution) into a DMA buffer; the
shortest period gives the bus clock, the median gives the nominal duty and
the longest low gives the stretch.

### Register map (T12-T24)

| ID | Check |
|----|-------|
| T12 | magic `0x5A`, version 1 |
| T13 | `REG_DS18B20` matches the `0x28` ROM count |
| T14 | Dallas CRC-8 for every stored ROM |
| T15 | unused slots: temp `0x8000`, status `0xFF`, ROM all `0xFF` |
| T16 | writes outside `0x0F`, `0x60`, `0x61` are ignored |
| T17 | `REG_SPEED` values other than 0/1 are ignored |
| T18 | unknown `REG_CMD` value changes nothing |
| T19 | speed 0/1 readback |
| T20 | command written while busy runs later, map intact |
| T21 | 400 reads during active cycles: no torn or invalid snapshot |
| T22 | cycle counter monotonic |
| T23 | BUSY flag sets and clears |
| T24 | header read ACKs while the manager is busy |

### Sensors (T25-T34)

| ID | Check |
|----|-------|
| T25 | every OK temperature is inside -55..125 C |
| T26 | five samples within 2 C |
| T27 | selected scratchpad CRC-8 |
| T28 | selections 0..3 expose the matching scratchpad |
| T29 | selection 15 publishes `0xFF` x 9 |
| T30 | DS2431 bytes and status stable across cycles |
| T31 | `CMD=2` rereads DS2431 with the same result |
| T32 | three rescans find the same ROM set |
| T33 | rescan queued while busy does not disturb the map |
| T34 | DS2413 is enumerated but not counted as DS18B20 |

### Stress (T35-T42)

| ID | Check |
|----|-------|
| T35 | 10 000 map reads, 0 errors, max latency recorded |
| T36 | soak (`g_soak_seconds`, default 60 s) |
| T37 | 4096 reads at random addresses and lengths |
| T38 | 1024 writes to read-only addresses leave the map intact |
| T39 | 1000 mixed read/write/command/NACK operations |
| T40 | 100 NACKs, then a valid map |
| T41 | 1000 alternating NACK/ACK transactions |
| T42 | final map valid and cycle still advancing |

## Expected result on the 9-device bench

```
T01 ... T42 PASS
SUMMARY: 42/42 passed, 0 failed
RESULT: PASS
```

T09 should report about 400 kHz with a 1.5-1.9 us low phase; T35 about 10 000
good reads and a maximum single read near 6 ms.

## Manual checks

- Power the AT32 board off: the tester must finish the current test with a
  timeout, not hang, and the LED must show a failure.
- Power it back on and send `run quick`; no Pico reset is needed.
- A scope on SCL is optional: the PIO capture already reports the frequency.
