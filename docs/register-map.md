# AT32F421F8P7 1-Wire bridge — register map

Canonical description of the I2C slave register file exposed by
`app/main.c` (`g_regs[256]`). Addresses are byte offsets within the
`0x42` slave (7-bit address `0x42`, transmitted as `0x84` write /
`0x85` read).

## Layout

| Address | Size | Name | Meaning |
|---------|------|------|---------|
| `0x00` | 1 | `REG_MAGIC` | `0x5A` — sanity byte the master should check first |
| `0x01` | 1 | `REG_VERSION` | Firmware version, `0x01` |
| `0x02` | 1 | `REG_FLAGS` | Status flags (see below) |
| `0x03` | 1 | `REG_DEVICES` | Total devices found by search (may exceed 16) |
| `0x04` | 1 | `REG_DS18B20` | DS18B20 count (may exceed 16) |
| `0x05` | 1 | `REG_ERROR` | Last error code |
| `0x06` | 2 | `REG_CYCLE` | Cycle counter, little-endian |
| `0x08` | 1 | `REG_SEQ` | Publish seqlock: odd while updating, even when stable |
| `0x0F` | 1 | `REG_SPEED` | `0` = 100 kHz, `1` = 400 kHz (writable) |
| `0x10..0x2F` | 32 | `REG_TEMP` | 16 × `int16` raw temperature (1/16 °C), `0x8000` = invalid |
| `0x30..0x3F` | 16 | `REG_TEMP_STATUS` | Per-sensor status (`OW_OK` / error code / `0xFF` invalid) |
| `0x40..0x47` | 8 | `REG_EEPROM` | Last DS2431 read block |
| `0x48` | 1 | `REG_EEPROM_STATUS` | DS2431 status (`0x00` = OK, `0xFF` = invalid) |
| `0x60` | 1 | `REG_CMD` | Command: `1` = rescan, `2` = reread DS2431, `3` = clear counters (writable) |
| `0x61` | 1 | `REG_SELECTED` | Low nibble selects the sensor whose scratchpad is exposed (writable) |
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

## Writable registers

Only `REG_SPEED` (`0x0F`), `REG_CMD` (`0x60`) and `REG_SELECTED` (`0x61`)
accept writes. A write is latched at STOP and applied when the current
transaction finishes; an aborted transfer (ACKFAIL/BUSERR/ARLOST/OUF)
discards the pending command or speed change.

Writes to every other address are ignored; the register pointer still
advances, so a block write cannot modify the read-only snapshot.
`REG_SPEED` accepts only `0` and `1`; other values are ignored.
`REG_SELECTED` keeps the low nibble only. A selection at or beyond the
DS18B20 count publishes `0xFF` in all nine scratchpad bytes instead of a
stale scratchpad.
`REG_CMD` is queued: if the manager is busy, the command runs when the
current cycle reaches its idle state, and a later command overwrites a
still-pending one.

## Source of truth

Register constants live in `src/manager.h` and are mirrored in
`app/main.c`, `tools/onewire_demo.py` and the RP2040 tester
(`tools/rp2040-tester/src/regmap.h`). Keep all copies in sync when the
map changes.
