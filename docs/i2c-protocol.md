# AT32F421 1-Wire bridge — I2C slave protocol

The bridge exposes a 256-byte register file as an I2C slave at 7-bit address
`0x42` (transmitted as `0x84` write / `0x85` read). The full register map is
in `docs/register-map.md`.

## Transaction rules

The I2C engine is polled (no interrupts) with clock stretching enabled. Every
superloop iteration calls `i2c_poll()` then `manager_poll()`; neither blocks.

### Write (master → bridge)

The master sends `[reg_addr, data_byte, ...]`. The first byte after the
address+W is the register pointer; subsequent bytes are written to consecutive
registers (pointer auto-increments, wraps at 0xFF).

Only `REG_SPEED` (`0x0F`), `REG_CMD` (`0x60`) and `REG_SELECTED` (`0x61`) are
writable; writes to any other address are ignored and the pointer still
advances. The value is **latched at STOP** and applied when the current
transaction finishes — it is **not** applied immediately on the byte write. An
aborted transfer (ACKFAIL/BUSERR/ARLOST/OUF) discards any latched command or
speed change.

### Read (master → bridge)

The master first writes the register pointer (a write transaction with no
data, or a combined write-then-read), then reads with address+R. The bridge
returns the register value at the pointer, auto-incrementing on each byte.

Use `REG_SEQ` (`0x08`) as a seqlock around a multi-byte read: read it
before and after the map, retry if it changed or is odd.

### Commands (`REG_CMD`)

| Value | Meaning |
|-------|---------|
| `1`   | Rescan the 1-Wire bus (search ROM) |
| `2`   | Re-read the DS2431 EEPROM block |
| `3`   | Clear cycle/error counters |

A command is queued if the manager is busy and runs when the current cycle
reaches its idle state; a later command overwrites a still-pending one.

### Speed (`REG_SPEED`)

| Value | SCL |
|-------|-----|
| `0`   | 100 kHz (standard mode) |
| `1`   | 400 kHz (fast mode) |

The prescaler is computed from the actual APB1 frequency:
`APB1/(2·SCL)` standard, `APB1/(3·SCL)` fast (1:2 high:low).

## Superloop timing

```
for (;;) {
    i2c_poll();      // serve any pending I2C transaction
    manager_poll();  // advance the 1-Wire state machine
}
```

The 1-Wire manager runs a non-blocking state machine: reset, write, read,
DS18B20 convert, DS2431 read. Each operation is scheduled as a hardware pass
on TMR1+DMA and completes asynchronously; `_poll` functions advance the state
machine by waiting for the timer update flag.

## Flags (`REG_FLAGS`)

| Bit | Mask | Set when |
|-----|------|----------|
| 0 | `0x01` | Search has completed |
| 1 | `0x02` | Search ended with an error |
| 2 | `0x04` | Manager busy (search/convert/read/EEPROM) |
| 3 | `0x08` | Register snapshot valid |
| 4 | `0x10` | Last DS2431 read OK |
| 5 | `0x20` | At least one temperature error this cycle |

## Invalid-slot markers

The bridge tracks the first 16 devices found by search. `REG_DEVICES`
reports the total (may exceed 16). Unused temperature slots read
`0x8000`/`0xFF` and unused ROM slots read all `0xFF` — a rescan that finds
fewer sensors does not leave stale data.
