#include "tests.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "crc8.h"
#include "crc16.h"
#include "regmap.h"

uint16_t map_crc16(const uint8_t *map)
{
    uint16_t crc = 0xFFFFu;

    for (int i = 0; i < 256; i++) {
        if ((i == (int)REG_FLAGS) || (i == (int)REG_SEQ) ||
            ((i >= (int)REG_FRAMES) && (i <= (int)REG_SPEED)) ||
            ((i >= (int)REG_CMD) && (i <= (int)REG_DIAG_REASON)) ||
            ((i >= (int)REG_MAX_STEP) && (i <= (int)REG_EEP_POLL))) {
            continue;
        }
        crc ^= (uint16_t)((uint16_t)map[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = ((crc & 0x8000u) != 0u)
                  ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

test_result_t g_results[MAX_RESULTS];
int           g_result_count;
bool          g_abort;
uint32_t      g_soak_seconds = 60u;

static int current = -1;

static void finish(bool ok, const char *fmt, va_list ap)
{
    if (current < 0) {
        return;
    }

    test_result_t *r = &g_results[current];

    r->ok = ok;
    if (fmt != NULL) {
        vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
    } else {
        r->detail[0] = '\0';
    }

    printf("%s %-22s %s%s%s\n", r->id, r->name,
           ok ? "PASS" : "FAIL",
           r->detail[0] ? " - " : "",
           r->detail);
    current = -1;
}

void tests_reset(void)
{
    memset(g_results, 0, sizeof(g_results));
    g_result_count = 0;
    g_abort = false;
    current = -1;
}

void t_start(const char *id, const char *name)
{
    if (g_result_count >= MAX_RESULTS) {
        current = -1;
        return;
    }

    current = g_result_count++;
    strncpy(g_results[current].id, id, sizeof(g_results[current].id) - 1u);
    g_results[current].id[sizeof(g_results[current].id) - 1u] = '\0';
    g_results[current].name = name;
    g_results[current].ok = false;
    g_results[current].detail[0] = '\0';
}

void t_pass(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    finish(true, fmt, ap);
    va_end(ap);
}

void t_fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    finish(false, fmt, ap);
    va_end(ap);
}

int map_stored_devices(const uint8_t *map)
{
    int dev = map[REG_DEVICES];

    return dev < (int)DEV_MAX ? dev : (int)DEV_MAX;
}

bool map_rom_equal(const uint8_t *a, const uint8_t *b)
{
    int n = map_stored_devices(a);

    if (n != map_stored_devices(b)) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (memcmp(&a[REG_ROMS + 8 * i], &b[REG_ROMS + 8 * i], 8u) != 0) {
            return false;
        }
    }
    return true;
}

bool map_validate(const uint8_t *map, char *why, size_t why_len)
{
#define REJECT(...) do { snprintf(why, why_len, __VA_ARGS__); return false; } while (0)

    if (map[REG_MAGIC] != 0x5Au) {
        REJECT("magic 0x%02X", map[REG_MAGIC]);
    }
    if ((map[REG_FLAGS] & FLAG_SNAPSHOT) == 0u) {
        REJECT("snapshot flag clear");
    }

    uint16_t crc = map_crc16(map);
    uint16_t crc_stored = (uint16_t)(map[REG_MAP_CRC_L] |
                                     ((uint16_t)map[REG_MAP_CRC_H] << 8));

    if (crc != crc_stored) {
        REJECT("map crc %04X != %04X", crc, crc_stored);
    }

    int devices = map[REG_DEVICES];
    int ds = map[REG_DS18B20];
    int stored = map_stored_devices(map);
    int fam28 = 0;

    for (int i = 0; i < stored; i++) {
        const uint8_t *rom = &map[REG_ROMS + 8 * i];

        if (crc8_dallas(rom, 7u) != rom[7]) {
            REJECT("rom %d crc", i);
        }
        if (rom[0] == FAMILY_DS18B20) {
            fam28++;
        }
    }

    for (int i = stored; i < (int)DEV_MAX; i++) {
        const uint8_t *rom = &map[REG_ROMS + 8 * i];

        for (int b = 0; b < 8; b++) {
            if (rom[b] != 0xFFu) {
                REJECT("slot %d not empty", i);
            }
        }
    }

    if (ds <= (int)DEV_MAX) {
        if (fam28 != ds) {
            REJECT("ds count %d != roms %d", ds, fam28);
        }
    } else if (fam28 < (int)DEV_MAX) {
        REJECT("ds count %d but only %d roms", ds, fam28);
    }
    if (devices < 0 || devices > 255) {
        REJECT("device count");
    }

    int ds_slots = ds < (int)DEV_MAX ? ds : (int)DEV_MAX;

    for (int i = 0; i < ds_slots; i++) {
        int16_t raw = (int16_t)((uint16_t)map[REG_TEMP + 2 * i] |
                                ((uint16_t)map[REG_TEMP + 2 * i + 1] << 8));
        uint8_t st = map[REG_TEMP_STATUS + i];

        if (st == ST_OK) {
            if (raw == (int16_t)INVALID_TEMP) {
                REJECT("sensor %d ok but 0x8000", i);
            }
            if ((raw < -55 * 16) || (raw > 125 * 16)) {
                REJECT("sensor %d temp %d", i, raw);
            }
        }
    }

    for (int i = ds_slots; i < (int)DEV_MAX; i++) {
        int16_t raw = (int16_t)((uint16_t)map[REG_TEMP + 2 * i] |
                                ((uint16_t)map[REG_TEMP + 2 * i + 1] << 8));

        if ((raw != (int16_t)INVALID_TEMP) ||
            (map[REG_TEMP_STATUS + i] != ST_INVALID)) {
            REJECT("unused sensor slot %d not invalid", i);
        }
    }

    return true;
#undef REJECT
}










