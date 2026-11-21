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

Line editor host test:

```sh
make -C test test
```

Flash: hold BOOTSEL, plug the Pico in, copy `build/i2c_tester.uf2` to the
`RPI-RP2` drive.

## Running

On power-up the tester waits up to 3 s for the USB console, probes `0x42`,
and runs the full suite. Commands:

```
run quick        T01-T34, about 40 s
run full         T01-T55, about 3 min
run soak N       full suite with the T36 soak extended to N seconds
results          print the last results again
abort            stop after the current test
info             tester version, SDK, clocks, pins, sampler rate
status           uptime, link speed, target, last run, snapshot summary
ping             probe 0x42 only
scan             probe every address 0x08..0x77
speed 100|400|auto   set the link; "400" requests 440 kHz, which measures 400 kHz
rescan           queue a 1-Wire ROM search
temp [n]         temperatures in C, all or one sensor
roms             ROM table with family and CRC-8
scratch [n]      select a sensor, wait a cycle, decode its scratchpad
eeprom           DS2431 bytes and status
dump [start] [len]   hex dump of the register map (default 0x00, 96 bytes)
reg <addr>       read one register
cmd <n>          write REG_CMD
select <n>       write REG_SELECTED
probe <addr>     probe one I2C address
recover          nine clocks and STOP on the I2C bus
health [clear]   bus health: transactions, missed frames, sticky flags, verdict
fault <code>      arm one REG_FAULT code (1, 3, 4, 5)
fuzz [ops] [seed] inject faults on both sides with a reproducible seed
stress [n]       n map reads at the current link speed, with error counters
sample [bytes]   measure SCL with PIO (kHz, min/med/max, stretch)
bench            100 vs 400 kHz on a 64-byte read
watch temp|status|cycle   1 Hz display, any key stops
adc              RP2040 VSYS and chip temperature
clear            clear the terminal
history [clear]  show or reset the command history
bootsel          reboot the Pico into the UF2 bootloader
? or help        command list; help <cmd> for details
```

The prompt is `> `. The firmware echoes typed characters and supports
Backspace, Left/Right, Home/End (`ESC [ H/F` or `1~/4~`), Delete (`3~`),
Ctrl-A/E/K/U/W/L. Up/Down walk the history, which starts preloaded with
every command above and keeps exact duplicates out: typing an existing
command moves it to the newest position. `history clear` restores the
preloaded list.

While a suite is running only Ctrl-C is read: it prints
`[abort requested]` and stops after the current test. Arrow keys and other
characters do not abort.

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

### Bus health (T43-T47)

The slave reports matched addresses and sticky error flags; the tester
compares them with its own transaction count.

| ID | Check |
|----|-------|
| T43 | 16 register reads advance the slave frame counter by 32-34 |
| T44 | `CMD_CLEAR_COUNTERS` zeroes the frame counter and the flags |
| T45 | 500 reads: transaction count equals matched frames, nothing missed |
| T46 | SDA held low for 50 ms is detected as stuck, and the target answers after release |
| T47 | 200 normal reads raise no health flag |

### Fault injection (T48-T54)

The tester injects faults from both sides and checks that the diagnostics
see them.

| ID | Check |
|----|-------|
| T48 | 32 bit-banged address corruptions land as exactly 32 missed frames |
| T49 | 8 dropped + 8 phantom frame counts move the counter by exactly that |
| T50 | a flipped bit in the map is rejected by `REG_MAP_CRC` |
| T51 | a dropped byte shifts the stream and the next read is clean |
| T52 | one extra clock on the bus leaves the map intact |
| T53 | 400 seeded mixed operations, no invariant failure |
| T54 | the same seed replays the same digest |
| T55 | corrupted writes: unknown command and speed ignored, out-of-range selection bounded |

## Expected result on the 9-device bench

```
T01 ... T55 PASS
SUMMARY: 55/55 passed, 0 failed
RESULT: PASS
```

T09 should report about 400 kHz with a 1.5-1.9 us low phase; T35 about 10 000
good reads and a maximum single read near 6 ms.

Acceptance on the bench: `run soak 1800` gives about 270 000 map reads
(1.6 MB per minute) with 0 invalid and about 0.02 % retried reads, max
latency 6.5 ms, and `health` reports CLEAN afterwards.

## Manual checks

- Power the AT32 board off: the tester must finish the current test with a
  timeout, not hang, and the LED must show a failure.
- Power it back on and send `run quick`; no Pico reset is needed.
- A scope on SCL is optional: the PIO capture already reports the frequency.
