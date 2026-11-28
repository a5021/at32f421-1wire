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
        (map[REG_MAGIC] == 0x5Au) && (map[REG_VERSION] == BRIDGE_VERSION)) {
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
    int retried = 0;

    for (int i = 0; i < 400; i++) {
        bool done = false;

        for (int attempt = 0; attempt < 3 && !done; attempt++) {
            uint8_t s1;
            uint8_t s2;

            if ((bus_read_reg(REG_SEQ, &s1, 1u) != 1) ||
                !map_read(alt) ||
                (bus_read_reg(REG_SEQ, &s2, 1u) != 1)) {
                retried++;
                continue;
            }
            if ((s1 != s2) || ((s1 & 1u) != 0u)) {
                torn++;
                done = true;
            } else if (!map_validate(alt, why, sizeof(why))) {
                retried++;
            } else {
                good++;
                done = true;
            }
        }
        if (!done) {
            invalid++;
        }
        if ((i % 32) == 0) {
            service();
            if (g_abort) {
                break;
            }
        }
    }
    if ((invalid == 0) && (good >= 300)) {
        t_pass("%d stable, %d torn, %d retried", good, torn, retried);    } else {
        t_fail("%d stable, %d torn, %d invalid", good, torn, invalid);
    }

    t_start("T22", "cycle monotonic");
    {
        uint16_t prev = 0xFFFFu;
        int inc = 0;
        int bad = 0;

        /* A queued rescan can keep the manager in search for the whole window
         * and publish nothing, so start from an idle cycle. */
        (void)map_wait_not_busy(5000u);

        for (int i = 0; i < 50; i++) {
            uint8_t raw[2];
            bool got = false;

            for (int attempt = 0; attempt < 3; attempt++) {
                if (bus_read_reg(REG_CYCLE_L, raw, 2u) == 2) {
                    got = true;
                    break;
                }
            }
            if (got) {
                uint16_t now = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));

                if (prev != 0xFFFFu) {
                    uint16_t delta = (uint16_t)(now - prev);

                    if (delta == 0u) {
                        /* idle wait */
                    } else if (delta <= 4u) {
                        inc++;
                    } else {
                        /* One corrupted byte can fake a jump; confirm once. */
                        uint8_t raw2[2];
                        bool got2 = false;

                        for (int a = 0; a < 2; a++) {
                            if (bus_read_reg(REG_CYCLE_L, raw2, 2u) == 2) {
                                got2 = true;
                                break;
                            }
                        }
                        if (got2) {
                            uint16_t now2 = (uint16_t)(raw2[0] |
                                                       ((uint16_t)raw2[1] << 8));
                            uint16_t d2 = (uint16_t)(now2 - prev);

                            if (d2 <= 4u) {
                                inc++;
                                now = now2;
                            } else {
                                bad++;
                                now = now2;
                            }
                        } else {
                            bad++;
                        }
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
            /* The last value makes a silent slave (0xFFFF) distinguishable
             * from a manager that simply is not publishing. */
            uint8_t mgr = 0xFFu;
            uint8_t fl = 0xFFu;

            (void)bus_read_reg(REG_MGR, &mgr, 1u);
            (void)bus_read_reg(REG_FLAGS, &fl, 1u);
            t_fail("%d increments, %d bad, last %u, mgr 0x%02X flags 0x%02X",
                   inc, bad, (unsigned)prev, mgr, fl);
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

    t_start("T43", "health frames");
    {
        uint8_t raw[4];
        uint32_t f1 = 0xFFFFFFFFu;
        uint32_t f2 = 0xFFFFFFFFu;
        bool ok = false;

        if (bus_read_reg(REG_FRAMES, raw, 4u) == 4) {
            f1 = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                 ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
            for (int i = 0; i < 16; i++) {
                uint8_t v;

                (void)bus_read_reg(REG_MAGIC, &v, 1u);
            }
            if (bus_read_reg(REG_FRAMES, raw, 4u) == 4) {
                f2 = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                     ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
                ok = true;
            }
        }

        uint32_t delta = f2 - f1;

        /* 16 register reads are 32 address phases; the sampling read that
         * follows adds one more before the counter is fetched. */
        if (ok && (delta >= 32u) && (delta <= 34u)) {
            t_pass("%u frames for 32 transactions", (unsigned)delta);
        } else {
            t_fail("frames %u -> %u (delta %u)", (unsigned)f1, (unsigned)f2,
                   (unsigned)delta);
        }
    }

    t_start("T44", "health clear");
    {
        uint8_t raw[5];
        bool ok = send_cmd(CMD_CLEAR_COUNTERS);

        sleep_ms(10u);
        if (ok && (bus_read_reg(REG_FRAMES, raw, sizeof(raw)) == (int)sizeof(raw))) {
            uint32_t frames = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                              ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);

            if ((frames <= 6u) && (raw[4] == 0u)) {
                t_pass("frames %u, flags 0", (unsigned)frames);
            } else {
                t_fail("frames %u, flags 0x%02X", (unsigned)frames, raw[4]);
            }
        } else {
            t_fail("clear or read failed");
        }
    }

    t_start("T45", "health accounting");
    {
        uint8_t v;
        int ok = 0;

        bus_health_reset();
        for (int i = 0; i < 500; i++) {
            if (bus_read_reg(REG_FLAGS, &v, 1u) == 1) {
                ok++;
            }
        }
        if (!bus_health_sample()) {
            t_fail("sample failed");
        } else if ((bus_health_missed <= 2u) && (bus_health_truncated <= 2u)) {
            t_pass("%d reads, %u missed, %u truncated", ok,
                   (unsigned)bus_health_missed,
                   (unsigned)bus_health_truncated);
        } else {
            t_fail("%u missed, %u truncated over %u txn",
                   (unsigned)bus_health_missed,
                   (unsigned)bus_health_truncated, (unsigned)bus_txn);
        }
    }

    t_start("T46", "health sda stuck policy");
    {
        (void)send_cmd(CMD_CLEAR_COUNTERS);
        sleep_ms(10u);

        bus_hold_sda_low(50u);
        sleep_ms(20u);

        /* Production builds detect and recover; DIAG_BUILD leaves the line
         * alone. Both policies must respond normally after the master releases
         * SDA. */
        uint8_t seen = 0u;

        for (int attempt = 0; attempt < 3; attempt++) {
            if (bus_health_sample()) {
                seen |= bus_health_flags;
            }
        }

        if (!bus_probe(BRIDGE_ADDR)) {
            t_fail("target did not respond after release, flags 0x%02X", seen);
        } else if ((seen & (HEALTH_SDA_STUCK | HEALTH_RECOVERED)) != 0u) {
            t_pass("detected/recovered, flags 0x%02X", seen);
        } else {
            t_pass("no auto-recovery, target responded");
        }
    }

    t_start("T46", "health open-frame timeout");
    {
        (void)send_cmd(CMD_CLEAR_COUNTERS);
        sleep_ms(10u);

        bool ack = bus_bb_abort_read(BRIDGE_ADDR, REG_MAGIC, 4);
        sleep_ms(1200u);

        uint8_t seen = 0u;

        for (int attempt = 0; attempt < 3; attempt++) {
            if (bus_health_sample()) {
                seen |= bus_health_flags;
            }
        }
        if (!ack) {
            t_fail("aborted read was not acknowledged");
        } else if (!bus_probe(BRIDGE_ADDR)) {
            t_fail("target did not respond after timeout, flags 0x%02X", seen);
        } else if ((seen & HEALTH_RECOVERED) != 0u) {
            t_pass("watchdog recovered, flags 0x%02X", seen);
        } else {
            t_pass("no watchdog, next address reset the frame");
        }
    }

    t_start("T46", "slow master pause");
    {
        (void)send_cmd(CMD_CLEAR_COUNTERS);
        sleep_ms(10u);

        bool ack = bus_bb_pause_write(BRIDGE_ADDR, REG_MAGIC, 1200u);
        uint8_t seen = 0u;

        for (int attempt = 0; attempt < 3; attempt++) {
            if (bus_health_sample()) {
                seen |= bus_health_flags;
            }
        }
        if (!ack) {
            t_fail("paused write was not acknowledged");
        } else if ((seen & (HEALTH_SDA_STUCK | HEALTH_RECOVERED |
                            HEALTH_RECOVERY_FAILED)) != 0u) {
            t_fail("recovery disturbed slow master, flags 0x%02X", seen);
        } else if (!bus_probe(BRIDGE_ADDR)) {
            t_fail("target did not respond after slow pause, flags 0x%02X", seen);
        } else {
            t_pass("no recovery during 1200 ms master pause, flags 0x%02X", seen);
        }
    }

    t_start("T46", "active abort PE cycle");
    {
        (void)send_cmd(CMD_CLEAR_COUNTERS);
        sleep_ms(10u);

        uint8_t full[256];
        uint8_t before[2] = {0, 0};
        uint8_t after[2] = {0, 0};
        bool armed = bus_fault(FAULT_ABORT);
        bool sampled_before = bus_read_reg(REG_DIAG_ABORT, before, 2u) == 2;
        absolute_time_t t0 = get_absolute_time();
        (void)bus_read_reg(REG_MAGIC, full, sizeof(full));
        bool sampled_after = bus_read_reg(REG_DIAG_ABORT, after, 2u) == 2;
        uint8_t magic = 0u;
        bool alive = bus_read_reg(REG_MAGIC, &magic, 1u) == 1;
        uint32_t elapsed = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());
        bool aborted = sampled_before && sampled_after &&
                       ((uint8_t)(after[0] - before[0]) != 0u);

        if (!armed) {
            t_fail("abort fault not armed");
        } else if (!aborted) {
            t_fail("active frame was not aborted, count %u -> %u",
                   before[0], after[0]);
        } else if (!alive || (magic != 0x5Au)) {
            t_fail("next frame failed after PE cycle, magic 0x%02X", magic);
        } else if (elapsed > 100000u) {
            t_fail("abort waited for watchdog, %u us", (unsigned)elapsed);
        } else {
            t_pass("PE cycle aborted active frame, next read in %u us",
                   (unsigned)elapsed);
        }
    }

    t_start("T47", "health no false alarm");
    {
        uint8_t v;

        (void)send_cmd(CMD_CLEAR_COUNTERS);
        sleep_ms(10u);
        for (int i = 0; i < 200; i++) {
            (void)bus_read_reg(REG_FLAGS, &v, 1u);
        }
        if (!bus_health_sample()) {
            t_fail("sample failed");
        } else if (bus_health_flags != 0u) {
            t_fail("flags 0x%02X under normal traffic", bus_health_flags);
        } else {
            t_pass("200 reads, no flags");
        }
    }
}
