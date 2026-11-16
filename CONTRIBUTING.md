# Contributing

## Build

### Firmware (ARM GCC)

```sh
make all               # build ELF, HEX, BIN
make size              # print flash/RAM usage
make clean             # remove build/
```

Requires `arm-none-eabi-gcc` on `PATH`.

### Host tests

```sh
make test              # build and run build/test_all
```

Host tests run on the host GCC and cover the protocol layer (`onewire.c`,
`ds18b20.c`, `ds2431.c`, `ow_crc.c`) and the manager state machine
(`src/manager.c`). They do **not** cover the I2C register bridge
(`app/main.c`), which touches real `I2C2` registers.

## Flashing

```sh
make flash-stlink      # via OpenOCD + ST-Link
make flash             # via pyOCD + CH341/WCH-Link
```

## Adding a register

Register constants live in `src/manager.h` and are duplicated in
`tools/onewire_demo.py` and documented in `docs/register-map.md`. Keep all
three in sync.

## Running on hardware

1. Connect PA10 to the 1-Wire bus (with 4.7 kΩ pull-up).
2. Connect PA0/PA1 to I2C2 (with external pull-ups).
3. Flash the firmware.
4. Run `python tools/onewire_demo.py` to read the bridge over CH341A I2C.
