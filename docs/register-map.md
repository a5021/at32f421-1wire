# AT32F421F8P7 1-Wire bridge — register map

Canonical description of the I2C slave register file exposed by
`app/main.c` (`g_regs[256]`). Addresses are byte offsets within the
`0x42` slave (7-bit address `0x42`, transmitted as `0x84` write /
`0x85` read).

## Layout

| Address | Size | Name | Meaning |
|---------|------|------|---------|
| `0x00` | 1 | `REG_MAGIC` | `0x5A` — sanity byte the master should check first |
| `0x01` | 1 | `REG_VERSION` | Firmware version, `0x03` |
| `0x02` | 1 | `REG_FLAGS` | Status flags (see below) |
| `0x03` | 1 | `REG_DEVICES` | Total devices found by search (may exceed 16) |
| `0x04` | 1 | `REG_DS18B20` | DS18B20 count (may exceed 16) |
| `0x05` | 1 | `REG_ERROR` | Last error code |
| `0x06` | 2 | `REG_CYCLE` | Cycle counter, little-endian |
| `0x08` | 1 | `REG_SEQ` | Publish seqlock: odd while updating, even when stable |
| `0x09` | 4 | `REG_FRAMES` | Matched I2C addresses, 32-bit little-endian |
| `0x0D` | 1 | `REG_HEALTH` | Sticky bus-health flags (see below) |
| `0x0E` | 1 | `REG_ERR_BUS` | `BUSERR` count, saturates at 255 |
| `0x0F` | 1 | `REG_SPEED` | `0` = 100 kHz, `1` = 400 kHz (writable) |
| `0x10..0x2F` | 32 | `REG_TEMP` | 16 × `int16` raw temperature (1/16 °C), `0x8000` = invalid |
| `0x30..0x3F` | 16 | `REG_TEMP_STATUS` | Per-sensor status (`OW_OK` / error code / `0xFF` invalid) |
| `0x40..0x47` | 8 | `REG_EEPROM` | Last DS2431 read block |
| `0x48` | 1 | `REG_EEPROM_STATUS` | DS2431 status (`0x00` = OK, `0xFF` = invalid) |
| `0x60` | 1 | `REG_CMD` | Command: `1` = rescan, `2` = reread DS2431, `3` = clear counters (writable) |
| `0x61` | 1 | `REG_SELECTED` | Low nibble selects the sensor whose scratchpad is exposed (writable) |
| `0x62` | 1 | `REG_FAULT` | One-shot fault injection, see below (writable) |
| `0x63` | 2 | `REG_MAP_CRC` | CRC-16/CCITT over the snapshot, little-endian |
| `0x70..0x78` | 9 | `REG_SCRATCH` | Selected DS18B20 scratchpad (9 bytes) |
| `0x80..0xFF` | 128 | `REG_ROMS` | 16 × 8-byte ROM table |

## Flags (`REG_FLAGS`)

| Bit | Mask | Name | Meaning |
|-----|------|------|---------|
| 0 | `0x01` | `FLAG_SEARCH_DONE` | A search has completed |
| 1 | `0x02` | `FLAG_SEARCH_ERROR` | Search ended with an error |
| 2 | `0x04` | `FLAG_BUSY` | Manager is busy (search/convert/read/EEPROM) |
| 3 | `0x08` | `FLAG_SNAPSHOT` | Register snapshot is valid |
| 4 | `0x10` | `FLAG_EEPROM_OK` | DS2431 read succeeded |
| 5 | `0x20` | `FLAG_TEMP_ERROR` | At least one temperature error this cycle |

## Invalid-slot markers

The bridge tracks the first 16 devices a search finds; `REG_DEVICES`
still reports the total.

| Slot | Temperature (`0x10 + 2i`) | Status (`0x30 + i`) | ROM (`0x80 + 8i`) |
|------|---------------------------|---------------------|-------------------|
| unused | `0x8000` (invalid) | `0xFF` | all `0xFF` |

## Snapshot consistency

`REG_SEQ` is a one-byte seqlock. `publish()` writes an odd value, updates
the snapshot, then writes the next even value. A master should read
`REG_SEQ`, read the map, read `REG_SEQ` again, and retry if the two differ
or the value is odd. This detects a publish that overlaps the transfer.

## Bus health (`0x09..0x0E`)

The slave cannot observe a NACK on its own address: if that frame is
corrupted the address simply does not match and no flag is raised. What it
can count is matched addresses, so the master compares its own transaction
count with `REG_FRAMES` and the difference is the number of address phases
the slave never saw.

The counter is 32 bits, so the host never has to handle a wrap. It only
changes at an address match, which happens before the master reads it, so
the four bytes cannot tear and no seqlock is needed.

| Bit | Mask | Name | Meaning |
|-----|------|------|---------|
| 0 | `0x01` | `HEALTH_BUSERR` | Illegal START/STOP position seen |
| 1 | `0x02` | `HEALTH_ARLOST` | Arbitration lost (noise or a second master) |
| 2 | `0x04` | `HEALTH_OUF` | Receive overrun |
| 3 | `0x08` | `HEALTH_SDA_STUCK` | SDA held low for 10 ms with no matched address |
| 4 | `0x10` | `HEALTH_RECOVERED` | Nine-clock recovery released the bus |

`ARLOST` has no counter on purpose: with one master it is already a
pathological event, so the flag is enough and a rate would add nothing.

The flags are sticky. After `HEALTH_SDA_STUCK` the slave disables the I2C
peripheral, drives nine clocks and a STOP on the pins in open-drain mode,
then re-initialises the peripheral with the same speed and address. It only
does this when SDA has been continuously low for 10 ms and no address
matched in that window, so a live transfer is never disturbed. A low SCL is
never treated as a fault: that is the slave's own clock stretch.

`CMD_CLEAR_COUNTERS` clears the counters, the flags and `REG_FRAMES`.

## Fault injection (`REG_FAULT`)

`REG_FAULT` accepts one code per write and fires once, on the next
transaction. It exists so a test can prove that corruption is detected
rather than silently accepted.

| Code | Effect |
|------|--------|
| 1 | invert bit 0 of the next transmitted byte |
| 2 | reserved (a long SCL stretch is not reliable on this slave) |
| 3 | drop the next byte from the stream |
| 4 | count one extra frame |
| 5 | drop one frame count |

Codes 4 and 5 give a digital ground truth for the health metric: the master
knows exactly how many frames were injected and can check that
`REG_FRAMES` moved by the same amount.

## Map checksum (`REG_MAP_CRC`)

CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`) over the snapshot, stored
little-endian. It is computed by `publish()` and lets the master detect a
corrupted byte in flight: I2C has no link CRC, so without this a single
flipped bit in a temperature would look like a plausible reading.

The following bytes are excluded and must be zeroed by the master before
computing the same CRC:

| Bytes | Why |
|-------|-----|
| `0x02` | `REG_FLAGS`: the busy bit toggles outside `publish()` |
| `0x08` | `REG_SEQ`: intentionally odd while a publish is in progress |
| `0x09..0x0F` | health block and `REG_SPEED`, injected at read time |
| `0x60..0x64` | command latch, selection, fault and the CRC itself |

Everything else (header counts, temperatures, statuses, EEPROM block,
scratchpad, ROM table) changes only inside `publish()`, so a stable `REG_SEQ`
guarantees the CRC matches.

## Writable registers

Only `REG_SPEED` (`0x0F`), `REG_CMD` (`0x60`), `REG_SELECTED` (`0x61`) and
`REG_FAULT` (`0x62`) accept writes. A write is latched at STOP and applied
when the current transaction finishes; an aborted transfer
(ACKFAIL/BUSERR/ARLOST/OUF) discards the pending command or speed change.

The health block is read-only. Its values are injected when a byte is
handed to the master, because `publish()` rebuilds `g_regs` from scratch on
every 1-Wire cycle.

Writes to every other address are ignored; the register pointer still
advances, so a block write cannot modify the read-only snapshot.
`REG_SPEED` accepts only `0` and `1`; other values are ignored.
`REG_SELECTED` keeps the low nibble only. A selection at or beyond the
DS18B20 count publishes `0xFF` in all nine scratchpad bytes instead of a
stale scratchpad.
`REG_CMD` is queued: if the manager is busy, the command runs when the
current cycle reaches its idle state, and a later command overwrites a
still-pending one. Only `1`, `2` and `3` are acted on; any other value is
ignored. A command byte corrupted in flight is **not detectable** because
`REG_CMD` is excluded from the map CRC, but the blast radius is bounded:
unknown commands do nothing, `REG_SPEED` accepts only `0`/`1`, and
`REG_SELECTED` masks to the low nibble and falls back to the invalid
scratchpad marker when out of range (T55).

## Source of truth

Register constants live in `src/manager.h` and are mirrored in
`app/main.c`, `tools/onewire_demo.py` and the RP2040 tester
(`tools/rp2040-tester/src/regmap.h`). Keep all copies in sync when the
map changes.
