#!/usr/bin/env python3
"""Read the AT32F421 1-Wire bridge over CH341A I2C (address 0x42)."""

import argparse
import ctypes
import sys
import time

DLL_PATH = r"C:\Windows\System32\CH341DLLA64.DLL"
ADDR7 = 0x42
ADDRW = ADDR7 << 1

REG_MAGIC = 0x00
REG_VERSION = 0x01
REG_FLAGS = 0x02
REG_DEVICES = 0x03
REG_DS18B20 = 0x04
REG_ERROR = 0x05
REG_CYCLE_L = 0x06
REG_CYCLE_H = 0x07
REG_SPEED = 0x0F
REG_TEMP = 0x10
REG_TEMP_STATUS = 0x30
REG_EEPROM = 0x40
REG_EEPROM_STATUS = 0x48
REG_CMD = 0x60
REG_SELECTED = 0x61
REG_SCRATCH = 0x70
REG_ROMS = 0x80

# NOTE: these register constants must stay in sync with docs/register-map.md
# and app/main.c (the REG_* defines). docs/register-map.md is the source of
# truth; update it first, then this file and the C header.

FLAG_SEARCH_DONE = 0x01
FLAG_SEARCH_ERROR = 0x02
FLAG_BUSY = 0x04
FLAG_SNAPSHOT = 0x08
FLAG_EEPROM_OK = 0x10
FLAG_TEMP_ERROR = 0x20

STATUS = {
    0: "ok",
    1: "busy",
    2: "no presence",
    3: "crc error",
    4: "param",
    5: "bus error",
    6: "end",
    0xFF: "-",
}

FAMILY = {
    0x28: "DS18B20",
    0x2D: "DS2431",
    0x3A: "DS2413",
}


class Bridge:
    def __init__(self, dll_path):
        self.dll = ctypes.WinDLL(dll_path)
        self.dll.CH341OpenDevice.argtypes = [ctypes.c_ulong]
        self.dll.CH341OpenDevice.restype = ctypes.c_ulong
        self.dll.CH341SetStream.argtypes = [ctypes.c_ulong, ctypes.c_ulong]
        self.dll.CH341StreamI2C.argtypes = [ctypes.c_ulong, ctypes.c_ulong,
                                            ctypes.c_void_p, ctypes.c_ulong,
                                            ctypes.c_void_p]
        self.dll.CH341StreamI2C.restype = ctypes.c_ulong
        self.dll.CH341CloseDevice.argtypes = [ctypes.c_ulong]

        handle = self.dll.CH341OpenDevice(0)
        if handle == 0:
            raise RuntimeError("CH341A open failed")
        if not self.dll.CH341SetStream(0, 1):
            raise RuntimeError("CH341A I2C mode failed")

    def close(self):
        self.dll.CH341CloseDevice(0)

    def write(self, reg, data):
        buf = (ctypes.c_ubyte * (2 + len(data)))(ADDRW, reg, *data)
        return bool(self.dll.CH341StreamI2C(0, len(buf), buf, 0, None))

    def read(self, reg, count):
        w = (ctypes.c_ubyte * 2)(ADDRW, reg)
        buf = (ctypes.c_ubyte * count)()
        ok = self.dll.CH341StreamI2C(0, 2, w, count, buf)
        return ok, bytes(buf)

    def read_map(self, size=256, chunk=256):
        out = bytearray()
        for off in range(0, size, chunk):
            n = min(chunk, size - off)
            ok, data = self.read(off, n)
            if not ok:
                raise RuntimeError("I2C read failed at 0x%02X" % off)
            out.extend(data)
        return bytes(out)

    def read_map_stable(self):
        last = None
        for _ in range(4):
            c1 = self.read(REG_CYCLE_L, 2)[1]
            m = self.read_map()
            c2 = self.read(REG_CYCLE_L, 2)[1]
            last = m
            if c1 == c2:
                break
        return last


def u16(buf, off):
    return buf[off] | (buf[off + 1] << 8)


def s16(buf, off):
    v = u16(buf, off)
    return v - 0x10000 if v & 0x8000 else v


def status_text(v):
    return STATUS.get(v, "0x%02X" % v)


def print_map(buf, raw=False, show_eeprom=False, show_scratch=False):
    magic = buf[REG_MAGIC]
    if magic != 0x5A:
        print("No bridge: magic=0x%02X (expected 0x5A)" % magic)
        print("first bytes:", buf[:16].hex(" "))
        return False

    flags = buf[REG_FLAGS]
    devices = buf[REG_DEVICES]
    ds_count = buf[REG_DS18B20]
    cycle = u16(buf, REG_CYCLE_L)
    speed = buf[REG_SPEED]

    print("AT32F421 1-Wire bridge  addr=0x%02X  fw=%d  devices=%d  ds18b20=%d  cycle=%d  i2c=%s" %
          (ADDR7, buf[REG_VERSION], devices, ds_count, cycle,
           "400k" if speed else "100k"))
    print("flags: search=%s%s snapshot=%s eeprom=%s temp=%s" % (
        "ok" if flags & FLAG_SEARCH_DONE else "-",
        " error" if flags & FLAG_SEARCH_ERROR else "",
        "valid" if flags & FLAG_SNAPSHOT else "-",
        "ok" if flags & FLAG_EEPROM_OK else "no",
        "error" if flags & FLAG_TEMP_ERROR else "ok"))

    ds_index = 0
    rows = min(devices, 16)
    print()
    print("#  ROM                          family   temp          status")
    for i in range(rows):
        rom = buf[REG_ROMS + 8 * i: REG_ROMS + 8 * i + 8]
        fam = rom[0]
        name = FAMILY.get(fam, "0x%02X" % fam)
        rom_s = " ".join("%02X" % b for b in rom)
        if fam == 0x28:
            st = buf[REG_TEMP_STATUS + ds_index]
            raw_t = s16(buf, REG_TEMP + 2 * ds_index)
            if raw_t == -32768:
                temp_s = "-"
            else:
                temp_s = "%.4f C" % (raw_t / 16.0)
                if raw:
                    temp_s += " (0x%04X)" % (raw_t & 0xFFFF)
            st_s = status_text(st)
            ds_index += 1
        elif fam == 0x2D:
            temp_s = "-"
            st_s = status_text(buf[REG_EEPROM_STATUS])
        else:
            temp_s = "-"
            st_s = "-"
        print("%-2d %-28s %-8s %-13s %s" % (i, rom_s, name, temp_s, st_s))

    if show_eeprom or (devices and any(buf[REG_ROMS + 8 * i] == 0x2D
                                        for i in range(rows))):
        eep = buf[REG_EEPROM: REG_EEPROM + 8]
        print("DS2431[0..7]: %s  status=%s" %
              (eep.hex(" ").upper(), status_text(buf[REG_EEPROM_STATUS])))

    if show_scratch:
        sp = buf[REG_SCRATCH: REG_SCRATCH + 9]
        sel = buf[REG_SELECTED] & 0x0F
        print("scratchpad[%d]: %s" % (sel, sp.hex(" ").upper()))

    if flags & FLAG_BUSY:
        print("(cycle in progress)")
    return True


def wait_snapshot(bridge, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = bridge.read_map_stable()
        if m[REG_MAGIC] == 0x5A and (m[REG_FLAGS] & FLAG_SNAPSHOT):
            return m
        time.sleep(0.1)
    return bridge.read_map_stable()


def main():
    ap = argparse.ArgumentParser(description="AT32F421 1-Wire bridge demo")
    ap.add_argument("--dll", default=DLL_PATH)
    ap.add_argument("--loop", type=int, default=1, help="refresh count")
    ap.add_argument("--raw", action="store_true", help="show raw temperature")
    ap.add_argument("--eeprom", action="store_true", help="show DS2431 bytes")
    ap.add_argument("--scratch", action="store_true", help="show selected scratchpad")
    ap.add_argument("--rescan", action="store_true", help="trigger ROM search")
    ap.add_argument("--speed", type=int, choices=(100, 400), help="set slave timing")
    args = ap.parse_args()

    try:
        bridge = Bridge(args.dll)
    except Exception as exc:
        print("CH341A error:", exc)
        return 1

    try:
        if args.rescan:
            bridge.write(REG_CMD, [1])
            time.sleep(2.5)
        if args.speed:
            bridge.write(REG_SPEED, [1 if args.speed == 400 else 0])
            time.sleep(0.1)

        buf = wait_snapshot(bridge)
        if not print_map(buf, args.raw, args.eeprom, args.scratch):
            return 1

        for _ in range(args.loop - 1):
            time.sleep(1.0)
            buf = bridge.read_map_stable()
            print()
            print_map(buf, args.raw, args.eeprom, args.scratch)
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
