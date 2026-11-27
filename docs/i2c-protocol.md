# AT32F421 1-Wire bridge — I2C slave protocol

The bridge exposes a 256-byte register file as an I2C slave at 7-bit address
`0x42` (transmitted as `0x84` write / `0x85` read). The full register map is
in `docs/register-map.md`.

## Transaction rules

The I2C engine is polled (no interrupts) with clock stretching enabled. Every
superloop iteration calls `i2c_poll()`. `manager_poll()` runs whenever no I2C
frame is active; no quiet-time gate is used.

Stretching is enabled by clearing `CTRL1.STRETCH` (bit 7): the bit is
`NOSTRETCH`, as on STM32, so `1` disables stretching. The official AT32F421
RM V2.05 confirms this polarity.

On this part `ADDR7F` clears only after a `STS1` read immediately followed by a
`STS2` read; the generated code keeps those reads adjacent. `STOPF` clears by
reading `STS1` and then writing `CTRL1`.

### Write (master → bridge)

The master sends `[reg_addr, data_byte, ...]`. The first byte after the
address+W is the register pointer; subsequent bytes are written to consecutive
registers (pointer auto-increments, wraps at 0xFF).

Only `REG_SPEED` (`0x0F`), `REG_CMD` (`0x60`), `REG_SELECTED` (`0x61`) and
`REG_FAULT` (`0x62`) are writable; writes to any other address are ignored and
the pointer still advances. `REG_SPEED`, `REG_CMD` and `REG_SELECTED` are
latched at STOP and applied when the current transaction finishes. An aborted
transfer (BUSERR/ARLOST/OUF) discards a latched command, speed change or
selection. `REG_FAULT` arms the next transaction.

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
`APB1/(2·SCL)` standard, `APB1/(3·SCL)` fast (1:2 high:low). A speed change is
latched at STOP but applied from the main loop after 50 us of idle-high bus,
so peripheral reinitialisation cannot race the next START. At 120 MHz APB1 the
100 kHz TRISE value clamps to 63 (about 533 ns), still below the standard-mode
1000 ns maximum.

## Superloop timing

```
for (;;) {
    i2c_poll();                 // serve any pending I2C transaction
    if (!i2c_active()) {
        manager_poll();         // advance the 1-Wire state machine
    }
}
```

The 1-Wire manager runs a non-blocking state machine: reset, write, read,
DS18B20 convert, DS2431 read. Each operation is scheduled as a hardware pass
on TMR1+DMA and completes asynchronously; `_poll` functions advance the state
machine by waiting for the timer update flag.

The slave may be addressed again before it has serviced the previous STOP: in
that case the pending STOP is finished at the address match, so a command or a
speed change is always applied to the transaction that carried it. The master
therefore needs no gap between the pointer write and the following read.

## Slave service latency

The slave ACKs an address in hardware, so the master starts clocking whether
or not the CPU is ready. If `i2c_poll()` does not load the data register
within about **8 bit times**, the master samples a released SDA line and the
byte is corrupted — the first bit comes back as 1 (`0x5A` reads as `0xDA`), or
a byte in mid-transfer is damaged.

The budget therefore scales with the bit time:

| SCL | Bit time | Budget | Measured |
|-----|----------|--------|----------|
| 400 kHz | 2.5 us | ~20 us | 1 retry in 1 000 short reads without a guard |
| 100 kHz | 10 us | ~80 us | 0 retries in 1 000 short reads without a guard |

Every step between two `i2c_poll()` calls must stay under that budget.
`publish()` used to be the longest one; it is now split into a shadow-buffer
fill, 32-byte CRC steps and a final 256-byte copy, so the worst single step is
much shorter. The host recovers from any overflow by re-reading, and the map
CRC makes the corruption visible instead of silent, so this is a latency
trade-off, not data loss.

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
