#include "tests.h"

#include <stdio.h>
#include <string.h>

#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"

/* Deterministic PRNG so a failing run can be replayed. */
static uint32_t fz_next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

/* The bridge must always come back: a fault may cost one read, never the
 * device. */
static bool fuzz_alive(void)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        uint8_t map[256];
        char why[80];

        if (map_stable(map, 3000u) && map_validate(map, why, sizeof(why))) {
            return true;
        }
        sleep_ms(20u);
    }
    return false;
}

static int fuzz_ops_failed[8];

int fuzz_run(uint32_t seed, uint32_t ops, uint32_t *digest)
{
    uint32_t d = seed;
    int fails = 0;

    memset(fuzz_ops_failed, 0, sizeof(fuzz_ops_failed));

    for (uint32_t i = 0; (i < ops) && !g_abort; i++) {
        uint32_t r = fz_next(&seed);
        int op = (int)(r % 8u);

        d = (d * 31u) + r;

        switch (op) {
        case 0: {
            uint8_t map[256];
            char why[80];
            bool valid = false;

            /* One corrupted frame is exactly what the protocol tolerates by
             * re-reading, so require recovery within two attempts. */
            for (int attempt = 0; (attempt < 2) && !valid; attempt++) {
                valid = map_stable(map, 3000u) &&
                        map_validate(map, why, sizeof(why));
            }
            if (!valid) {
                fails++;
                fuzz_ops_failed[op]++;
            }
            break;
        }
        case 1: {
            uint8_t v;

            if (bus_read_reg(REG_MAGIC, &v, 1u) != 1) {
                fails++;
                fuzz_ops_failed[op]++;
            }
            break;
        }
        case 2:
            if (bus_bb_corrupt_addr(BRIDGE_ADDR, 3)) {
                fails++;            /* nothing may answer that address */
                fuzz_ops_failed[op]++;
            }
            break;
        case 3:
            (void)bus_bb_truncate(BRIDGE_ADDR, REG_TEMP, 1 + (int)(r % 7u));
            break;
        case 4:
            (void)bus_fault(FAULT_EXTRA_FRAME);
            break;
        case 5:
            (void)bus_fault(FAULT_BIT_FLIP);
            break;
        case 6:
            (void)bus_fault(FAULT_DROP_FRAME);
            break;
        default:
            if (!bus_probe(BRIDGE_ADDR)) {
                fails++;
                fuzz_ops_failed[op]++;
            }
            break;
        }

        if ((i % 4u) == 0u) {
            if (!fuzz_alive()) {
                fails++;
                fuzz_ops_failed[op]++;
            }
        }
        if ((i % 64u) == 0u) {
            service();
        }
    }

    *digest = d;
    return fails;
}

void suite_fault(void)
{
    t_start("T48", "inject address");
    {
        int acks = 0;

        bus_health_reset();
        for (int i = 0; i < 32; i++) {
            if (bus_bb_corrupt_addr(BRIDGE_ADDR, 5)) {
                acks++;
            }
        }
        if (!bus_health_sample()) {
            t_fail("sample failed");
        } else if ((acks == 0) && (bus_health_missed == 32u)) {
            t_pass("32 lost address phases");
        } else {
            t_fail("missed %u, wrong-address acks %d",
                   (unsigned)bus_health_missed, acks);
        }
    }

    t_start("T49", "inject frames");
    {
        uint8_t v;
        bool ok = true;

        bus_health_reset();
        for (int i = 0; i < 8; i++) {
            if (!bus_fault(FAULT_DROP_FRAME)) {
                ok = false;
            }
            (void)bus_read_reg(REG_MAGIC, &v, 1u);
        }
        if (!bus_health_sample()) {
            ok = false;
        }
        uint32_t dropped = bus_health_missed;

        bus_health_reset();
        for (int i = 0; i < 8; i++) {
            if (!bus_fault(FAULT_EXTRA_FRAME)) {
                ok = false;
            }
        }
        if (!bus_health_sample()) {
            ok = false;
        }
        int32_t surplus = (int32_t)bus_health_frames - (int32_t)bus_txn;

        if (ok && (dropped == 8u) && (surplus == 8) && (bus_health_missed == 0u)) {
            t_pass("8 dropped, 8 phantom, no false loss");
        } else {
            t_fail("dropped %u, surplus %d, missed %u", (unsigned)dropped,
                   (int)surplus, (unsigned)bus_health_missed);
        }
    }

    t_start("T50", "map crc");
    {
        uint8_t rot[256];
        uint8_t map[256];
        char why[80];

        if (!map_stable(map, 3000u)) {
            t_fail("map not stable before injection");
        } else if (!bus_fault(FAULT_BIT_FLIP)) {
            t_fail("fault not armed");
        } else if (bus_read_reg(REG_TEMP, rot, sizeof(rot)) != (int)sizeof(rot)) {
            t_fail("rotated map read failed");
        } else {
            for (int i = 0; i < 256; i++) {
                map[(uint8_t)(i + REG_TEMP)] = rot[i];
            }
            if (map_validate(map, why, sizeof(why))) {
                t_fail("flipped bit accepted");
            } else if (strstr(why, "crc") == NULL) {
                t_fail("rejected for another reason: %s", why);
            } else {
                t_pass("%s", why);
            }
        }
    }

    t_start("T51", "inject skip");
    {
        uint8_t v[4] = { 0 };
        uint8_t after = 0;

        /* The skipped byte is the first one, so the stream starts at VERSION
         * instead of MAGIC. */
        if (!bus_fault(FAULT_SKIP_BYTE)) {
            t_fail("fault not armed");
        } else if (bus_read_reg(REG_MAGIC, v, sizeof(v)) != (int)sizeof(v)) {
            t_fail("read failed");
        } else if ((v[0] == 0x5Au) || (v[0] != BRIDGE_VERSION)) {
            t_fail("first byte %02X, expected version %02X", v[0], BRIDGE_VERSION);
        } else if ((bus_read_reg(REG_MAGIC, &after, 1u) != 1) ||
                   (after != 0x5Au)) {
            t_fail("next read not clean: %02X", after);
        } else if (!fuzz_alive()) {
            t_fail("bridge not healthy after the skip");
        } else {
            t_pass("first byte dropped, next read clean");
        }
    }

    t_start("T52", "inject clock");
    {
        uint8_t map[256];
        char why[80];

        bus_bb_extra_clock();
        sleep_ms(5u);
        if (map_stable(map, 3000u) && map_validate(map, why, sizeof(why))) {
            t_pass("one extra clock, map intact");
        } else {
            t_fail("map invalid after extra clock: %s", why);
        }
    }

    t_start("T53", "random fuzz");
    {
        uint32_t digest = 0u;
        int fails = fuzz_run(0x1F35C0DEu, 400u, &digest);

        if (fails == 0) {
            t_pass("400 ops, digest %08X", (unsigned)digest);
        } else {
            t_fail("%d fails %d/%d/%d/%d/%d/%d/%d/%d digest %08X", fails,
                   fuzz_ops_failed[0], fuzz_ops_failed[1],
                   fuzz_ops_failed[2], fuzz_ops_failed[3],
                   fuzz_ops_failed[4], fuzz_ops_failed[5],
                   fuzz_ops_failed[6], fuzz_ops_failed[7], (unsigned)digest);
        }
    }

    t_start("T54", "fuzz replay");
    {
        uint32_t d1 = 0u;
        uint32_t d2 = 0u;

        int f1 = fuzz_run(0x00C0FFEEu, 64u, &d1);
        int f2 = fuzz_run(0x00C0FFEEu, 64u, &d2);

        if ((d1 == d2) && (f1 == 0) && (f2 == 0) && fuzz_alive()) {
            t_pass("same seed, same digest %08X", (unsigned)d1);
        } else {
            t_fail("digest %08X/%08X, fails %d/%d", (unsigned)d1, (unsigned)d2,
                   f1, f2);
        }
    }

    t_start("T55", "corrupt writes");
    {
        uint8_t map[256];
        char why[80];
        uint8_t speed0 = 0xFF;
        uint8_t speed1 = 0xFF;
        uint8_t sp[9] = { 0 };
        bool ok = map_stable(map, 3000u) && map_validate(map, why, sizeof(why));

        if (ok) {
            ok = bus_read_reg(REG_SPEED, &speed0, 1u) == 1;
        }

        /* Unknown command (3 with bit 4 flipped = 0x13) must be ignored. */
        if (ok) {
            ok = bus_bb_corrupt_write(BRIDGE_ADDR, REG_CMD, CMD_CLEAR_COUNTERS,
                                      4);
            sleep_ms(30u);
        }
        /* Out-of-range speed (1 with bit 3 flipped = 0x09) must be ignored. */
        if (ok) {
            ok = bus_bb_corrupt_write(BRIDGE_ADDR, REG_SPEED, 1u, 3);
            sleep_ms(30u);
        }
        if (ok) {
            ok = bus_read_reg(REG_SPEED, &speed1, 1u) == 1;
        }
        /* Selection 0x0F with bit 3 flipped = 0x07: outside the sensor table,
         * so the published scratchpad must be the invalid marker.  The
         * scratchpad is republished once per cycle, hence the wait. */
        if (ok) {
            uint8_t raw[2] = { 0, 0 };

            ok = bus_read_reg(REG_CYCLE_L, raw, 2u) == 2;
            uint16_t cycle = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));

            if (ok) {
                ok = bus_bb_corrupt_write(BRIDGE_ADDR, REG_SELECTED, 0x0Fu, 3);
            }
            if (ok) {
                ok = map_wait_cycle(cycle, map, 4000u);
            }
            if (ok) {
                ok = bus_read_reg(REG_SCRATCH, sp, sizeof(sp)) ==
                     (int)sizeof(sp);
            }
        }

        (void)select_sensor(0u);

        if (!ok) {
            t_fail("injection failed");
        } else if (speed0 > 1u) {
            t_fail("speed readback %u", speed0);
        } else if (speed1 != speed0) {
            t_fail("speed changed %u -> %u", speed0, speed1);
        } else {
            bool all_ff = true;

            for (int i = 0; i < 9; i++) {
                if (sp[i] != 0xFFu) {
                    all_ff = false;
                }
            }
            if (!all_ff) {
                t_fail("selection 7 published a real scratchpad");
            } else if (!fuzz_alive()) {
                t_fail("bridge not healthy after corrupted writes");
            } else {
                t_pass("unknown cmd and speed ignored, selection bounded");
            }
        }
    }

    /* The injected faults are expected; leave the diagnostics clean so the
     * next `health` reflects real traffic, not this suite. */
    (void)send_cmd(CMD_CLEAR_COUNTERS);
    bus_stats_reset();
    sleep_ms(10u);
}
