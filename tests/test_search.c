#include "test.h"
#include "ow_hal_mock.h"
#include "onewire.h"
#include "ow_crc.h"

static void make_rom(uint8_t rom[8], uint8_t family, uint8_t a, uint8_t b,
                     uint8_t c, uint8_t d, uint8_t e)
{
    rom[0] = family;
    rom[1] = a;
    rom[2] = b;
    rom[3] = c;
    rom[4] = d;
    rom[5] = e;
    rom[6] = (uint8_t)(a ^ b ^ c);
    rom[7] = ow_crc8(rom, 7u);
}

static int enumerate(uint8_t roms[][8], int max, ow_status_t *last,
                     ow_search_t *state)
{
    ow_search_t s;
    int found = 0;
    int guard = 40000;
    ow_status_t st;

    ow_search_begin(&s);
    for (;;) {
        st = ow_search_poll(&s);
        if (st == OW_BUSY) {
            if (guard-- <= 0) {
                *last = OW_BUS_ERROR;
                break;
            }
            continue;
        }
        if (st == OW_OK) {
            if (found < max) {
                memcpy(roms[found], s.rom, 8u);
            }
            found++;
            continue;
        }
        *last = st;
        break;
    }
    if (state != 0) {
        *state = s;
    }
    return found;
}

static bool contains(uint8_t roms[][8], int n, const uint8_t *rom)
{
    int i;

    for (i = 0; i < n; i++) {
        if (memcmp(roms[i], rom, 8u) == 0) {
            return true;
        }
    }
    return false;
}

void test_search_run(void)
{
    static const uint8_t one[8] = {
        0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
    };
    uint8_t roms[8][8];
    ow_status_t last;
    ow_search_t s;
    int n;

    /* Single device: one ROM, then OW_END, with last_device set. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, one);
    n = enumerate(roms, 8, &last, &s);
    TF_CHECK_EQ(1, n);
    TF_CHECK_EQ(OW_END, last);
    TF_CHECK_MEM(one, roms[0], 8);
    TF_CHECK(s.last_device);

    /* Three devices: all found exactly once, regardless of order. */
    mock_reset();
    {
        uint8_t a[8], b[8], c[8];

        make_rom(a, 0x28, 0x11, 0x22, 0x33, 0x44, 0x55);
        make_rom(b, 0x28, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE);
        make_rom(c, 0x28, 0x01, 0x02, 0x03, 0x04, 0x05);
        (void)mock_add(MOCK_DS18B20, a);
        (void)mock_add(MOCK_DS2431, b);
        (void)mock_add(MOCK_DS18B20, c);

        n = enumerate(roms, 8, &last, 0);
        TF_CHECK_EQ(3, n);
        TF_CHECK_EQ(OW_END, last);
        TF_CHECK(contains(roms, n, a));
        TF_CHECK(contains(roms, n, b));
        TF_CHECK(contains(roms, n, c));
        TF_CHECK(!contains(&roms[0], n, (const uint8_t *)"\0\0\0\0\0\0\0\0"));
    }

    /* Presence lost after one valid ROM must not look like normal end. */
    mock_reset();
    {
        uint8_t a[8], b[8];
        ow_status_t st;

        make_rom(a, 0x28, 0x21, 0x22, 0x23, 0x24, 0x25);
        make_rom(b, 0x28, 0x31, 0x32, 0x33, 0x34, 0x35);
        (void)mock_add(MOCK_DS18B20, a);
        (void)mock_add(MOCK_DS18B20, b);
        ow_search_begin(&s);
        n = 0;
        {
            int guard = 40000;

            for (;;) {
                st = ow_search_poll(&s);
                if (st == OW_BUSY) {
                    if (guard-- <= 0) {
                        st = OW_BUS_ERROR;
                        break;
                    }
                    continue;
                }
                if (st == OW_OK) {
                    n++;
                    if (n == 1) {
                        mock_set_presence(false);
                    }
                    continue;
                }
                break;
            }
        }
        TF_CHECK_EQ(1, n);
        TF_CHECK_EQ(OW_NO_PRESENCE, st);
    }

    /* No devices -> OW_END straight away. */
    mock_reset();
    n = enumerate(roms, 8, &last, 0);
    TF_CHECK_EQ(0, n);
    TF_CHECK_EQ(OW_END, last);

    /* Bad CRC -> OW_CRC_ERROR, enumeration stops. */
    mock_reset();
    {
        uint8_t bad[8];

        make_rom(bad, 0x28, 0x10, 0x20, 0x30, 0x40, 0x50);
        bad[7] ^= 0x01u;
        (void)mock_add(MOCK_DS18B20, bad);
        n = enumerate(roms, 8, &last, 0);
        TF_CHECK_EQ(OW_CRC_ERROR, last);
    }

    /* Eight devices, all unique. */
    mock_reset();
    {
        uint8_t all[8][8];
        uint32_t lcg = 0xDEADBEEFu;
        int i;

        for (i = 0; i < 8; i++) {
            lcg = (lcg * 1664525u) + 1013904223u;
            make_rom(all[i], 0x28,
                     (uint8_t)(lcg >> 24), (uint8_t)(lcg >> 16),
                     (uint8_t)(lcg >> 8), (uint8_t)lcg,
                     (uint8_t)(lcg ^ 0x5Au));
            all[i][6] = (uint8_t)i;          /* force uniqueness */
            all[i][7] = ow_crc8(all[i], 7u);
            (void)mock_add((i & 1) ? MOCK_DS2431 : MOCK_DS18B20, all[i]);
        }

        n = enumerate(roms, 8, &last, 0);
        TF_CHECK_EQ(8, n);
        TF_CHECK_EQ(OW_END, last);
        for (i = 0; i < 8; i++) {
            TF_CHECK(contains(roms, n, all[i]));
        }
        for (i = 0; i < n; i++) {
            int j;

            for (j = i + 1; j < n; j++) {
                TF_CHECK(memcmp(roms[i], roms[j], 8u) != 0);
            }
        }
    }

    /* A fresh search after a complete pass finds the same set again. */
    n = enumerate(roms, 8, &last, 0);
    TF_CHECK_EQ(8, n);
    TF_CHECK_EQ(OW_END, last);
}
