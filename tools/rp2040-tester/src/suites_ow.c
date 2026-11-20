#include "tests.h"

#include <stdio.h>
#include <string.h>

#include "crc8.h"
#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"

static int find_family(const uint8_t *map, uint8_t family)
{
    for (int i = 0; i < map_stored_devices(map); i++) {
        if (map[REG_ROMS + 8 * i] == family) {
            return i;
        }
    }
    return -1;
}

static bool wait_cycles(uint16_t *cycle, int n, uint32_t timeout_ms)
{
    uint8_t tmp[256];

    for (int i = 0; i < n; i++) {
        if (!map_wait_cycle(*cycle, tmp, timeout_ms)) {
            return false;
        }
        *cycle = map_cycle(tmp);
    }
    return true;
}

static int16_t temp_at(const uint8_t *map, int i)
{
    return (int16_t)((uint16_t)map[REG_TEMP + 2 * i] |
                     ((uint16_t)map[REG_TEMP + 2 * i + 1] << 8));
}

void suite_ow(void)
{
    uint8_t map[256];
    uint8_t alt[256];
    char why[80];

    t_start("T25", "temperature range");
    if (map_stable(map, 3000u) && map_validate(map, why, sizeof(why))) {
        int ds = map[REG_DS18B20];
        int ok = 0;
        int16_t lo = 32767;
        int16_t hi = -32768;

        for (int i = 0; i < ds && i < (int)DEV_MAX; i++) {
            if (map[REG_TEMP_STATUS + i] == ST_OK) {
                int16_t t = temp_at(map, i);

                ok++;
                if (t < lo) {
                    lo = t;
                }
                if (t > hi) {
                    hi = t;
                }
            }
        }
        if (ok == 0) {
            t_fail("no valid sensors");
        } else {
            t_pass("%d sensors, %d..%d counts", ok, lo, hi);
        }
    } else {
        t_fail("%s", why);
    }

    t_start("T26", "temperature stability");
    if (map_stable(map, 3000u)) {
        int ds = map[REG_DS18B20];
        int bad = 0;
        int samples = 0;

        for (int s = 0; s < 5; s++) {
            sleep_ms(100u);
            if (!map_stable(alt, 3000u)) {
                bad++;
                continue;
            }
            samples++;
            for (int i = 0; i < ds && i < (int)DEV_MAX; i++) {
                if ((map[REG_TEMP_STATUS + i] == ST_OK) &&
                    (alt[REG_TEMP_STATUS + i] == ST_OK)) {
                    int diff = (int)temp_at(alt, i) - (int)temp_at(map, i);

                    if (diff < 0) {
                        diff = -diff;
                    }
                    if (diff > 32) {
                        bad++;
                    }
                }
            }
        }
        if (bad == 0 && samples == 5) {
            t_pass("5 samples within 2 C");
        } else {
            t_fail("%d deviations", bad);
        }
    } else {
        t_fail("no snapshot");
    }

    t_start("T27", "scratchpad crc");
    if (map_stable(map, 3000u) && (map[REG_DS18B20] != 0u)) {
        uint16_t cycle = map_cycle(map);
        uint8_t sp[9];

        select_sensor(0u);
        if (wait_cycles(&cycle, 2, 8000u) && read_scratch(sp) &&
            (crc8_dallas(sp, 8u) == sp[8])) {
            t_pass("%02X %02X %02X ...", sp[0], sp[1], sp[2]);
        } else {
            t_fail("bad scratchpad");
        }
    } else {
        t_pass("no sensors");
    }

    t_start("T28", "selected scratchpad");
    {
        int ds = map[REG_DS18B20];
        int n = ds < 4 ? ds : 4;
        int bad = 0;

        for (int i = 0; i < n; i++) {
            uint16_t cycle;
            uint8_t sp[9];

            if (!map_stable(map, 3000u)) {
                bad++;
                break;
            }
            cycle = map_cycle(map);
            select_sensor((uint8_t)i);
            if (!wait_cycles(&cycle, 2, 8000u) || !read_scratch(sp) ||
                (crc8_dallas(sp, 8u) != sp[8]) ||
                ((sp[4] & 0x1Fu) != 0x1Fu)) {
                bad++;
            }
        }
        select_sensor(0u);
        if (bad == 0) {
            t_pass("%d sensors", n);
        } else {
            t_fail("%d bad selections", bad);
        }
    }

    t_start("T29", "invalid selection");
    {
        uint16_t cycle;
        uint8_t sp[9];
        bool ok = false;

        if (map_stable(map, 3000u)) {
            cycle = map_cycle(map);
            select_sensor(0x0Fu);
            if (wait_cycles(&cycle, 2, 8000u) && read_scratch(sp)) {
                ok = true;
                for (int i = 0; i < 9; i++) {
                    if (sp[i] != 0xFFu) {
                        ok = false;
                    }
                }
            }
            select_sensor(0u);
        }
        if (ok) {
            t_pass("0xFF scratchpad");
        } else {
            t_fail("stale or short");
        }
    }

    t_start("T30", "ds2431 stable");
    if (!map_stable(map, 3000u)) {
        t_fail("no snapshot");
    } else {
        int idx = find_family(map, FAMILY_DS2431);

        if (idx < 0) {
            t_pass("absent");
        } else {
            uint8_t data[8];
            uint8_t st = map[REG_EEPROM_STATUS];
            uint16_t cycle = map_cycle(map);

            memcpy(data, &map[REG_EEPROM], 8u);
            if (wait_cycles(&cycle, 2, 8000u) && map_stable(alt, 3000u) &&
                (memcmp(data, &alt[REG_EEPROM], 8u) == 0) &&
                (alt[REG_EEPROM_STATUS] == st) && (st == ST_OK)) {
                t_pass("%02X %02X %02X ...", data[0], data[1], data[2]);
            } else {
                t_fail("data or status changed");
            }
        }
    }

    t_start("T31", "ds2431 reread");
    if (!map_stable(map, 3000u)) {
        t_fail("no snapshot");
    } else {
        int idx = find_family(map, FAMILY_DS2431);

        if (idx < 0) {
            t_pass("absent");
        } else {
            uint8_t data[8];
            uint16_t cycle = map_cycle(map);

            memcpy(data, &map[REG_EEPROM], 8u);
            send_cmd(CMD_REREAD_EEPROM);
            if (map_wait_cycle(cycle, alt, 15000u) &&
                (memcmp(data, &alt[REG_EEPROM], 8u) == 0) &&
                (alt[REG_EEPROM_STATUS] == ST_OK)) {
                t_pass("unchanged, status ok");
            } else {
                t_fail("reread mismatch");
            }
        }
    }

    t_start("T32", "rescan stable");
    {
        int bad = 0;

        if (!map_stable(map, 3000u)) {
            t_fail("no snapshot");
        } else {
            for (int k = 0; k < 3; k++) {
                uint16_t cycle = map_cycle(map);

                send_cmd(CMD_RESCAN);
                if (!map_wait_cycle(cycle, alt, 20000u) ||
                    !map_rom_equal(map, alt)) {
                    bad++;
                }
                memcpy(map, alt, sizeof(map));
                service();
                if (g_abort) {
                    break;
                }
            }
            if (bad == 0) {
                t_pass("3 rescans identical");
            } else {
                t_fail("%d changed", bad);
            }
        }
    }

    t_start("T33", "rescan while busy");
    {
        uint8_t dev = map[REG_DEVICES];
        int bad = 0;

        if (!map_stable(map, 3000u)) {
            t_fail("no snapshot");
        } else {
            dev = map[REG_DEVICES];
            if (!map_wait_busy(5000u)) {
                bad++;
            } else {
                uint16_t cycle = map_cycle(map);

                send_cmd(CMD_RESCAN);
                if (!map_wait_cycle(cycle, alt, 25000u) ||
                    (alt[REG_DEVICES] != dev) || !map_rom_equal(map, alt)) {
                    bad++;
                }
            }
            if (bad == 0) {
                t_pass("deferred, %u devices", dev);
            } else {
                t_fail("count or roms changed");
            }
        }
    }

    t_start("T34", "ds2413 presence");
    if (!map_stable(map, 3000u)) {
        t_fail("no snapshot");
    } else {
        int idx = find_family(map, FAMILY_DS2413);

        if (idx < 0) {
            t_pass("absent");
        } else if (map[REG_DS18B20] <= map[REG_DEVICES]) {
            t_pass("enumerated, no pio driver");
        } else {
            t_fail("counts inconsistent");
        }
    }
}
