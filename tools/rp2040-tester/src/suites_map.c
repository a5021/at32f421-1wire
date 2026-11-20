#include "tests.h"

#include <stdio.h>
#include <string.h>

#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"
#include "crc8.h"

static int16_t temp_at(const uint8_t *map, int i)
{
    return (int16_t)((uint16_t)map[REG_TEMP + 2 * i] |
                     ((uint16_t)map[REG_TEMP + 2 * i + 1] << 8));
}

void suite_map(void)
{
    uint8_t map[256];
    uint8_t alt[256];
    char why[80];

    map_stable(map, 3000u);

    t_start("T12", "magic + version");
    if (map_stable(map, 3000u) &&
        (map[REG_MAGIC] == 0x5Au) && (map[REG_VERSION] == 1u)) {
        t_pass("0x5A v1");
    } else {
        t_fail("bad header");
    }

    t_start("T13", "device counts");
    int fam28 = 0;

    for (int i = 0; i < map_stored_devices(map); i++) {
        if (map[REG_ROMS + 8 * i] == FAMILY_DS18B20) {
            fam28++;
        }
    }
    if (fam28 == (int)map[REG_DS18B20]) {
        t_pass("%u total, %d x 0x28", map[REG_DEVICES], fam28);
    } else {
        t_fail("count %u != %d roms", map[REG_DS18B20], fam28);
    }

    t_start("T14", "rom crc8");
    int bad = 0;

    for (int i = 0; i < map_stored_devices(map); i++) {
        const uint8_t *rom = &map[REG_ROMS + 8 * i];

        if (crc8_dallas(rom, 7u) != rom[7]) {
            bad++;
        }
    }
    if (bad == 0) {
        t_pass("%d roms", map_stored_devices(map));
    } else {
        t_fail("%d bad", bad);
    }

    t_start("T15", "invalid markers");
    if (map_validate(map, why, sizeof(why))) {
        t_pass("unused slots 0x8000/0xFF/FF");
    } else {
        t_fail("%s", why);
    }

    t_start("T16", "read-only writes");
    if (!map_stable(map, 3000u)) {
        t_fail("no snapshot");
    } else {
        uint8_t v = 0xAAu;
        uint8_t rom0[8];
        uint8_t devices = map[REG_DEVICES];
        int16_t t0 = temp_at(map, 0);
        uint16_t cyc = map_cycle(map);

        memcpy(rom0, &map[REG_ROMS], 8u);

        bus_write_reg(REG_MAGIC, &v, 1u);
        bus_write_reg(REG_FLAGS, &v, 1u);
        bus_write_reg(REG_DEVICES, &v, 1u);
        bus_write_reg(REG_TEMP, &v, 1u);
        bus_write_reg(REG_ROMS, &v, 1u);

        bool rejected = true;

        for (int retry = 0; retry < 5; retry++) {
            if (!map_read(alt) || (map_cycle(alt) != cyc)) {
                sleep_ms(50u);
                continue;
            }
            rejected = (alt[REG_MAGIC] == 0x5Au) &&
                       (alt[REG_DEVICES] == devices) &&
                       (memcmp(&alt[REG_ROMS], rom0, 8u) == 0) &&
                       (map[REG_DS18B20] == 0u || temp_at(alt, 0) == t0) &&
                       ((alt[REG_FLAGS] & ~FLAG_BUSY) ==
                        (map[REG_FLAGS] & ~FLAG_BUSY));
            break;
        }
        if (rejected) {
            t_pass("writes ignored");
        } else {
            t_fail("snapshot accepted a write");
        }
    }

    t_start("T17", "invalid speed");
    if (map_stable(map, 3000u)) {
        uint8_t before = map[REG_SPEED];
        uint8_t v = 5u;
        uint8_t after = 0xEEu;

        bus_write_reg(REG_SPEED, &v, 1u);
        v = 0xAAu;
        bus_write_reg(REG_SPEED, &v, 1u);
        sleep_ms(30u);
        bus_read_reg(REG_SPEED, &after, 1u);
        if (after == before) {
            t_pass("5 and 0xAA ignored (%u)", before);
        } else {
            t_fail("%u -> %u", before, after);
        }
    } else {
        t_fail("no snapshot");
    }

    t_start("T18", "unknown command");
    if (map_stable(map, 3000u)) {
        uint8_t dev = map[REG_DEVICES];

        send_cmd(0x7Fu);
        sleep_ms(100u);
        if (map_stable(alt, 3000u) &&
            (alt[REG_DEVICES] == dev) && (alt[REG_MAGIC] == 0x5Au)) {
            t_pass("ignored");
        } else {
            t_fail("snapshot changed");
        }
    } else {
        t_fail("no snapshot");
    }

    t_start("T19", "speed readback");
    bool sp_ok = true;

    bus_set_speed(BUS_SPEED_100K);
    write_speed(0u);
    sleep_ms(50u);
    uint8_t sp = 0xEEu;
    bus_read_reg(REG_SPEED, &sp, 1u);
    sp_ok = sp_ok && (sp == 0u);

    write_speed(1u);
    sleep_ms(50u);
    bus_set_speed(BUS_SPEED_400K);
    bus_read_reg(REG_SPEED, &sp, 1u);
    sp_ok = sp_ok && (sp == 1u);

    if (sp_ok) {
        t_pass("0 then 1");
    } else {
        t_fail("readback %u", sp);
    }

    t_start("T20", "command while busy");
    if (!map_stable(map, 3000u) || !map_wait_busy(5000u)) {
        t_fail("never busy");
    } else {
        uint8_t dev = map[REG_DEVICES];

        send_cmd(CMD_CLEAR_COUNTERS);
        if (map_wait_not_busy(10000u)) {
            sleep_ms(400u);
            if (map_stable(alt, 3000u) && (alt[REG_DEVICES] == dev) &&
                (alt[REG_MAGIC] == 0x5Au)) {
                t_pass("deferred, map intact");
            } else {
                t_fail("map changed");
            }
        } else {
            t_fail("still busy");
        }
    }

    t_start("T21", "no torn snapshots");
    int torn = 0;
    int invalid = 0;
    int good = 0;

    for (int i = 0; i < 400; i++) {
        uint8_t s1;
        uint8_t s2;

        if ((bus_read_reg(REG_SEQ, &s1, 1u) != 1) ||
            !map_read(alt) ||
            (bus_read_reg(REG_SEQ, &s2, 1u) != 1)) {
            invalid++;
        } else if ((s1 != s2) || ((s1 & 1u) != 0u)) {
            torn++;
        } else if (!map_validate(alt, why, sizeof(why))) {
            invalid++;
        } else {
            good++;
        }
        if ((i % 32) == 0) {
            service();
            if (g_abort) {
                break;
            }
        }
    }
    if ((invalid == 0) && (good >= 300)) {
        t_pass("%d stable, %d torn", good, torn);    } else {
        t_fail("%d stable, %d torn, %d invalid", good, torn, invalid);
    }

    t_start("T22", "cycle monotonic");
    {
        uint16_t prev = 0xFFFFu;
        int inc = 0;
        int bad = 0;

        for (int i = 0; i < 20; i++) {
            uint8_t raw[2];

            if (bus_read_reg(REG_CYCLE_L, raw, 2u) == 2) {
                uint16_t now = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));

                if (prev != 0xFFFFu) {
                    uint16_t delta = (uint16_t)(now - prev);

                    if (delta == 0u) {
                        /* idle wait */
                    } else if (delta <= 2u) {
                        inc++;
                    } else {
                        bad++;
                    }
                }
                prev = now;
            } else {
                bad++;
            }
            sleep_ms(100u);
        }
        if ((bad == 0) && (inc > 0)) {
            t_pass("%d increments", inc);
        } else {
            t_fail("%d increments, %d bad", inc, bad);
        }
    }

    t_start("T23", "busy transitions");
    {
        bool saw_busy = false;
        bool saw_idle = false;
        int transitions = 0;
        bool last = false;

        for (int i = 0; i < 3000; i++) {
            uint8_t flags;

            if (bus_read_reg(REG_FLAGS, &flags, 1u) == 1) {
                bool busy = (flags & FLAG_BUSY) != 0u;

                if (busy) {
                    saw_busy = true;
                } else {
                    saw_idle = true;
                }
                if (busy != last) {
                    transitions++;
                    last = busy;
                }
            }
            sleep_us(1000u);
        }
        if (saw_busy && saw_idle && (transitions >= 2)) {
            t_pass("%d transitions", transitions);
        } else {
            t_fail("busy %d idle %d transitions %d", saw_busy, saw_idle,
                   transitions);
        }
    }

    t_start("T24", "header while busy");
    if (!map_wait_busy(5000u)) {
        t_fail("never busy");
    } else {
        uint8_t hdr[16];

        if ((bus_read_reg(REG_MAGIC, hdr, 16u) == 16) &&
            (hdr[REG_MAGIC] == 0x5Au)) {
            t_pass("ack during cycle");
        } else {
            t_fail("read failed");
        }
    }
}
