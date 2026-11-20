#include "tests.h"

#include <stdio.h>
#include <string.h>

#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"
#include "scl_probe.h"

#define MEASURE_LEN     32u
#define SAMPLER_PIN     5u

static scl_stats_t stats400;
static scl_stats_t stats100;
static uint32_t    us400;
static uint32_t    us100;
static bool        measured400;
static bool        measured100;

static bool measure_link(uint32_t master_hz, uint8_t slave_speed,
                         scl_stats_t *st, uint32_t *elapsed_us)
{
    uint8_t rom[MEASURE_LEN];

    /* 100 kHz works with either slave timing, so use it to apply the new
     * slave configuration before changing the master clock. */
    bus_set_speed(BUS_SPEED_100K);
    if (!write_speed(slave_speed)) {
        return false;
    }
    sleep_ms(30);
    bus_set_speed(master_hz);
    sleep_ms(30);

    if (!map_wait_not_busy(5000u)) {
        return false;
    }

    scl_probe_start();
    absolute_time_t t0 = get_absolute_time();
    int got = bus_read_reg(REG_ROMS, rom, sizeof(rom));
    uint32_t words = scl_probe_stop();
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    if (elapsed_us != NULL) {
        *elapsed_us = (uint32_t)us;
    }
    if (got != (int)sizeof(rom)) {
        return false;
    }
    return scl_probe_analyze(words, st);
}

void suite_bus(void)
{
    uint8_t map[256];
    char why[80];

    t_start("T01", "probe 0x42");
    bool ok = false;

    for (int i = 0; i < 3 && !ok; i++) {
        ok = bus_probe(BRIDGE_ADDR);
        if (!ok) {
            sleep_ms(20u);
        }
    }
    if (ok) {
        t_pass("ack");
    } else {
        t_fail("no ack");
    }

    t_start("T02", "nack unused");
    int acked = 0;

    for (uint8_t addr = 0x50u; addr <= 0x52u; addr++) {
        if (bus_probe(addr)) {
            acked++;
        }
    }
    if (bus_probe(0x7Fu)) {
        acked++;
    }
    if (acked == 0) {
        t_pass("0x50-0x52,0x7F nack");
    } else {
        t_fail("%d addresses acked", acked);
    }

    t_start("T03", "header");
    uint8_t hdr[16];

    if ((bus_read_reg(REG_MAGIC, hdr, sizeof(hdr)) == (int)sizeof(hdr)) &&
        (hdr[REG_MAGIC] == 0x5Au)) {
        t_pass("magic 0x5A version %u flags 0x%02X", hdr[REG_VERSION],
               hdr[REG_FLAGS]);
    } else {
        t_fail("short or bad header");
    }

    t_start("T04", "map 256");
    if (map_stable(map, 3000u) && map_validate(map, why, sizeof(why))) {
        t_pass("%u devices, %u ds18b20, cycle %u", map[REG_DEVICES],
               map[REG_DS18B20], map_cycle(map));
    } else {
        t_fail("%s", why);
    }

    t_start("T05", "pointer increment");
    uint8_t a[8];
    uint8_t b[8];

    if ((map_stable(map, 3000u)) &&
        (bus_read_reg(REG_EEPROM, a, 8u) == 8) &&
        (bus_read_reg(REG_ROMS, b, 8u) == 8) &&
        (memcmp(a, &map[REG_EEPROM], 8u) == 0) &&
        (memcmp(b, &map[REG_ROMS], 8u) == 0)) {
        t_pass("0x40, 0x80 match");
    } else {
        t_fail("mismatch");
    }

    t_start("T06", "pointer wrap");
    uint8_t w[4];

    if ((map_stable(map, 3000u)) &&
        (bus_read_reg(0xFEu, w, 4u) == 4) &&
        (w[0] == map[0xFEu]) && (w[1] == map[0xFFu]) &&
        (w[2] == map[0x00u]) && (w[3] == map[0x01u])) {
        t_pass("0xFE..0x01 wraps");
    } else {
        t_fail("wrap mismatch");
    }

    t_start("T07", "repeated start");
    uint8_t rs[8];

    if ((map_stable(map, 3000u)) &&
        (bus_read_reg_nostop(REG_ROMS, rs, 8u) == 8) &&
        (memcmp(rs, &map[REG_ROMS], 8u) == 0)) {
        uint8_t again[8];

        if (bus_read_reg(REG_ROMS, again, 8u) == 8) {
            t_pass("Sr read ok");
        } else {
            t_fail("bus not recovered after Sr");
        }
    } else {
        bus_recover();
        t_fail("Sr read failed");
    }

    t_start("T08", "turnaround sweep");
    static const uint32_t gaps[] = { 0u, 2u, 5u, 10u, 20u, 50u };
    int bad = 0;

    if (!map_stable(map, 3000u)) {
        t_fail("no snapshot");
    } else {
        for (unsigned i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
            uint8_t r[8];

            if (!bus_write_reg(REG_ROMS, NULL, 0u)) {
                bad++;
                continue;
            }
            if (gaps[i] != 0u) {
                busy_wait_us_32(gaps[i]);
            }
            if (bus_read_only(r, 8u) != 8) {
                bad++;
                continue;
            }
            if (memcmp(r, &map[REG_ROMS], 8u) != 0) {
                bad++;
            }
        }
        if (bad == 0) {
            t_pass("0..50 us gap");
        } else {
            t_fail("%d gaps failed", bad);
        }
    }

    measured400 = measure_link(BUS_SPEED_400K, 1u, &stats400, &us400);
    measured100 = measure_link(BUS_SPEED_100K, 0u, &stats100, &us100);

    t_start("T09", "400 kHz clock");
    if (!measured400) {
        t_fail("measurement failed");
    } else {
        int khz = (int)(stats400.khz_fast + 0.5f);
        int low = (int)(stats400.min_low_us * 1000.0f + 0.5f);
        int per = (int)(stats400.med_period_us * 1000.0f + 0.5f);

        if ((khz >= 390) && (khz <= 410) && (stats400.periods >= 100u) &&
            (low > 0) && (low < per)) {
            t_pass("%d kHz, low %d/%d ns, %u periods", khz, low, per,
                   stats400.periods);
        } else {
            t_fail("%d kHz, low %d/%d ns, %u periods", khz, low, per,
                   stats400.periods);
        }
    }

    t_start("T10", "stretch");
    if (!measured400) {
        t_fail("no 400 kHz capture");
    } else {
        int max_low = (int)(stats400.max_low_us + 0.5f);

        if (max_low < 5000) {
            t_pass("max low %d us", max_low);
        } else {
            t_fail("max low %d us", max_low);
        }
    }

    t_start("T11", "100/400 ratio");
    if (!measured100 || !measured400 || (us400 == 0u)) {
        t_fail("measurement failed");
    } else {
        int ratio10 = (int)((10u * us100) / us400);

        if ((ratio10 >= 30) && (ratio10 <= 50)) {
            t_pass("%u us vs %u us, %d.%dx", us100, us400, ratio10 / 10,
                   ratio10 % 10);
        } else {
            t_fail("%u us vs %u us, %d.%dx", us100, us400, ratio10 / 10,
                   ratio10 % 10);
        }
    }

    /* Leave the link at 400 kHz for the remaining suites. */
    bus_set_speed(BUS_SPEED_400K);
    write_speed(1u);
    sleep_ms(30);
}
