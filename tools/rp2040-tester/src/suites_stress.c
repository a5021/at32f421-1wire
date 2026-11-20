#include "tests.h"

#include <stdio.h>
#include <string.h>

#include "crc8.h"
#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"

static uint32_t rng_state = 0x12345678u;

static uint32_t rnd(void)
{
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int read_map_checked(uint32_t *torn, uint32_t *invalid,
                            uint32_t *max_us)
{
    uint8_t map[256];
    uint8_t c1[2];
    uint8_t c2[2];
    absolute_time_t t0 = get_absolute_time();

    if ((bus_read_reg(REG_CYCLE_L, c1, 2u) != 2) ||
        !map_read(map) ||
        (bus_read_reg(REG_CYCLE_L, c2, 2u) != 2)) {
        (*invalid)++;
        return -1;
    }

    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    if ((uint32_t)us > *max_us) {
        *max_us = (uint32_t)us;
    }

    if ((c1[0] != c2[0]) || (c1[1] != c2[1])) {
        (*torn)++;
        return 1;
    }

    char why[80];

    if (!map_validate(map, why, sizeof(why))) {
        (*invalid)++;
        return -1;
    }
    return 0;
}

void suite_stress(void)
{
    uint8_t map[256];
    uint8_t alt[256];
    char why[80];

    bus_set_speed(BUS_SPEED_400K);
    write_speed(1u);
    sleep_ms(50u);

    t_start("T35", "10k map reads");
    {
        uint32_t torn = 0u;
        uint32_t invalid = 0u;
        uint32_t max_us = 0u;
        uint32_t good = 0u;
        uint32_t errors = 0u;

        for (uint32_t i = 0u; i < 10000u; i++) {
            int r = read_map_checked(&torn, &invalid, &max_us);

            if (r < 0) {
                errors++;
            } else if (r == 0) {
                good++;
            }
            if ((i % 128u) == 0u) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if ((errors == 0u) && (invalid == 0u) && (good >= 9900u)) {
            t_pass("%u good, %u torn, max %u us", good, torn, max_us);
        } else {
            t_fail("%u good, %u torn, %u invalid, %u errors", good, torn,
                   invalid, errors);
        }
    }

    t_start("T36", "60 s soak");
    {
        uint32_t torn = 0u;
        uint32_t invalid = 0u;
        uint32_t max_us = 0u;
        uint32_t good = 0u;
        uint32_t errors = 0u;
        absolute_time_t deadline = make_timeout_time_ms(g_soak_seconds * 1000u);

        while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
            int r = read_map_checked(&torn, &invalid, &max_us);

            if (r < 0) {
                errors++;
            } else if (r == 0) {
                good++;
            }
            service();
            if (g_abort) {
                break;
            }
        }
        if ((errors == 0u) && (invalid == 0u) && (good > 0u)) {
            t_pass("%u good, %u torn, max %u us", good, torn, max_us);
        } else {
            t_fail("%u good, %u torn, %u invalid, %u errors", good, torn,
                   invalid, errors);
        }
    }

    t_start("T37", "random reads");
    {
        uint32_t errors = 0u;

        for (int i = 0; i < 4096; i++) {
            uint8_t buf[16];
            uint32_t r = rnd();
            uint8_t reg = (uint8_t)(r & 0xFFu);
            size_t len = 1u + ((r >> 8) % 16u);

            if (bus_read_reg(reg, buf, len) != (int)len) {
                errors++;
            }
            if ((i % 256) == 0) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if ((errors == 0u) && map_stable(map, 3000u) &&
            (map[REG_MAGIC] == 0x5Au)) {
            t_pass("4096 reads");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T38", "random read-only writes");
    {
        uint8_t rom0[8];
        uint8_t eep[8];
        uint8_t dev = 0u;
        uint8_t ds = 0u;
        uint32_t errors = 0u;

        map_stable(map, 3000u);
        memcpy(rom0, &map[REG_ROMS], 8u);
        memcpy(eep, &map[REG_EEPROM], 8u);
        dev = map[REG_DEVICES];
        ds = map[REG_DS18B20];

        for (int i = 0; i < 1024; i++) {
            uint32_t r = rnd();
            uint8_t reg = (uint8_t)(r & 0xFFu);
            uint8_t v = (uint8_t)(r >> 8);

            if ((reg == REG_SPEED) || (reg == REG_CMD) ||
                (reg == REG_SELECTED)) {
                continue;
            }
            if (bus_write_reg(reg, &v, 1u) != 1) {
                errors++;
            }
        }

        if ((errors == 0u) && map_stable(alt, 3000u) &&
            (alt[REG_MAGIC] == 0x5Au) && (alt[REG_DEVICES] == dev) &&
            (alt[REG_DS18B20] == ds) &&
            (memcmp(&alt[REG_ROMS], rom0, 8u) == 0) &&
            (memcmp(&alt[REG_EEPROM], eep, 8u) == 0)) {
            t_pass("snapshot intact");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T39", "mixed fuzz");
    {
        uint32_t errors = 0u;

        for (int i = 0; i < 1000; i++) {
            uint32_t r = rnd();
            uint8_t buf[16];

            switch (r & 7u) {
            case 0u:
                if (bus_read_reg(REG_MAGIC, buf, 16u) != 16) {
                    errors++;
                }
                break;
            case 1u: {
                uint8_t full[256];

                if (!map_read(full)) {
                    errors++;
                }
                break;
            }
            case 2u:
                if (!send_cmd(CMD_CLEAR_COUNTERS)) {
                    errors++;
                }
                break;
            case 3u:
                if (!send_cmd(0x7Eu)) {
                    errors++;
                }
                break;
            case 4u:
                if (bus_probe(0x50u)) {
                    errors++;
                }
                break;
            case 5u:
                if (bus_read_reg(REG_SCRATCH, buf, 9u) != 9) {
                    errors++;
                }
                break;
            case 6u: {
                uint8_t v = 7u;

                if (!bus_write_reg(REG_SPEED, &v, 1u)) {
                    errors++;
                }
                break;
            }
            default:
                if (!bus_probe(BRIDGE_ADDR)) {
                    errors++;
                }
                break;
            }

            if ((i % 100) == 99) {
                if (!map_stable(alt, 3000u) ||
                    !map_validate(alt, why, sizeof(why))) {
                    errors++;
                }
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if (errors == 0u) {
            t_pass("1000 mixed ops");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T40", "nack recovery");
    {
        int errors = 0;

        for (int i = 0; i < 100; i++) {
            if (bus_probe(0x50u)) {
                errors++;
            }
        }
        if ((errors == 0) && map_stable(map, 3000u) &&
            map_validate(map, why, sizeof(why))) {
            t_pass("100 nacks, map ok");
        } else {
            t_fail("%d unexpected acks", errors);
        }
    }

    t_start("T41", "nack storm");
    {
        int errors = 0;

        for (int i = 0; i < 1000; i++) {
            if (bus_probe(0x50u) || !bus_probe(BRIDGE_ADDR)) {
                errors++;
            }
            if ((i % 200) == 0) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if (errors == 0) {
            t_pass("1000 alternations");
        } else {
            t_fail("%d errors", errors);
        }
    }

    t_start("T42", "final validation");
    {
        why[0] = '\0';
        bool ok = map_stable(map, 3000u) &&
                  map_validate(map, why, sizeof(why));
        uint16_t cycle = ok ? map_cycle(map) : 0u;

        sleep_ms(1500u);
        ok = ok && map_stable(alt, 3000u) &&
             (map_cycle(alt) != cycle);

        if (ok) {
            t_pass("cycle advancing, map valid");
        } else {
            t_fail("%s", why);
        }
    }

    bus_set_speed(BUS_SPEED_400K);
    write_speed(1u);
}
