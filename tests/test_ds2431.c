#include "test.h"
#include "ow_hal_mock.h"
#include "ds2431.h"

static const uint8_t rom[8] = {
    0x2D, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x00
};

static ow_status_t run_read(ds2431_t *d, const uint8_t *r, uint16_t addr,
                            uint8_t *dst, uint8_t len)
{
    int guard = 400;
    ow_status_t st;

    if (!ds2431_read_start(d, r, addr, dst, len)) {
        return OW_PARAM;
    }
    while (((st = ds2431_read_poll(d)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

static ow_status_t run_write(ds2431_t *d, const uint8_t *r, uint16_t addr,
                             const uint8_t *src)
{
    int guard = 400;
    ow_status_t st;

    if (!ds2431_write_start(d, r, addr, src)) {
        return OW_PARAM;
    }
    while (((st = ds2431_write_poll(d)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

void test_ds2431_run(void)
{
    ds2431_t d;
    uint8_t buf[OW_MAX_BYTES];
    uint8_t data[8];

    /* Read Memory has no CRC: Skip ROM, 0xF0, TA1, TA2, raw bytes. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        int i;

        for (i = 0; i < 8; i++) {
            dev->mem[i] = (uint8_t)(0xA0 + i);
        }
        ds2431_init(&d);
        TF_CHECK_EQ(OW_OK, run_read(&d, 0, 0u, buf, 8u));
        for (i = 0; i < 8; i++) {
            TF_CHECK_EQ(0xA0 + i, buf[i]);
        }
        TF_CHECK_EQ(-1, mock_last_wait_us());
        {
            int n = 0;
            const uint8_t *tx = mock_last_tx(&n);

            TF_CHECK_EQ(4, n);
            TF_CHECK_EQ(0xCC, tx[0]);
            TF_CHECK_EQ(0xF0, tx[1]);
            TF_CHECK_EQ(0x00, tx[2]);
            TF_CHECK_EQ(0x00, tx[3]);
        }
    }

    /* A busy engine must not finish the read early (regression: the state
     * machine used to mark PH_DONE while ow_read_poll still returned BUSY). */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        uint8_t out[8];
        int guard = 400;
        ow_status_t st;

        memset(dev->mem, 0x3C, 8u);
        ds2431_init(&d);
        mock_set_busy_ticks(2);
        TF_CHECK(ds2431_read_start(&d, 0, 0u, out, 8u));
        while (((st = ds2431_read_poll(&d)) == OW_BUSY) && (guard-- > 0)) {
        }
        TF_CHECK_EQ(OW_OK, st);
        TF_CHECK_MEM(dev->mem, out, 8);
        TF_CHECK(!mock_busy());
    }

    /* Last aligned block and the 32-byte request limit. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        int i;

        for (i = 0; i < 8; i++) {
            dev->mem[1016 + i] = (uint8_t)(0x10 + i);
        }
        ds2431_init(&d);
        TF_CHECK_EQ(OW_OK, run_read(&d, 0, 1016u, buf, 8u));
        for (i = 0; i < 8; i++) {
            TF_CHECK_EQ(0x10 + i, buf[i]);
        }
        TF_CHECK_EQ(OW_OK, run_read(&d, 0, 0u, buf, OW_MAX_BYTES));
        TF_CHECK_EQ(OW_PARAM, run_read(&d, 0, 1020u, buf, 8u));
        TF_CHECK_EQ(OW_PARAM, run_read(&d, 0, 0u, buf, 0u));
        TF_CHECK_EQ(OW_PARAM, run_read(&d, 0, 1021u, buf, 4u));
    }

    /* Match ROM read: 0x55 + ROM + 0xF0 + TA. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        int i;

        for (i = 0; i < 8; i++) {
            dev->mem[0x40 + i] = (uint8_t)(0x60 + i);
        }
        ds2431_init(&d);
        TF_CHECK_EQ(OW_OK, run_read(&d, rom, 0x40u, buf, 8u));
        TF_CHECK_MEM(dev->mem + 0x40, buf, 8);
        {
            int n = 0;
            const uint8_t *tx = mock_last_tx(&n);

            TF_CHECK_EQ(12, n);
            TF_CHECK_EQ(0x55, tx[0]);
            TF_CHECK_MEM(rom, &tx[1], 8);
            TF_CHECK_EQ(0xF0, tx[9]);
            TF_CHECK_EQ(0x40, tx[10]);
            TF_CHECK_EQ(0x00, tx[11]);
        }
    }

    /* Full write cycle: Write Scratchpad, verify, Copy, status 0xAA. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        int i;

        for (i = 0; i < 8; i++) {
            data[i] = (uint8_t)(0x30 + i);
        }
        ds2431_init(&d);
        TF_CHECK_EQ(OW_OK, run_write(&d, 0, 0x10u, data));
        TF_CHECK_MEM(data, dev->mem + 0x10, 8);
        TF_CHECK_MEM(data, dev->sp31, 8);
        TF_CHECK_EQ(0x10, dev->latched_ta);
        TF_CHECK_EQ(7, dev->es);
        TF_CHECK_EQ(0xAA, d.status);
        TF_CHECK_EQ(11000, mock_last_wait_us());
        TF_CHECK(mock_last_cmd_strong());
        TF_CHECK(!mock_read_while_strong());
        TF_CHECK(!mock_strong());
        {
            int n = 0;
            const uint8_t *tx = mock_last_tx(&n);

            TF_CHECK_EQ(5, n);
            TF_CHECK_EQ(0xCC, tx[0]);
            TF_CHECK_EQ(0x55, tx[1]);
            TF_CHECK_EQ(0x10, tx[2]);
            TF_CHECK_EQ(0x00, tx[3]);
            TF_CHECK_EQ(0x07, tx[4]);
        }
    }

    /* Match ROM write. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);
        int i;

        for (i = 0; i < 8; i++) {
            data[i] = (uint8_t)(0x80 + i);
        }
        ds2431_init(&d);
        TF_CHECK_EQ(OW_OK, run_write(&d, rom, 0x20u, data));
        TF_CHECK_MEM(data, dev->mem + 0x20, 8);
        {
            int n = 0;
            const uint8_t *tx = mock_last_tx(&n);

            TF_CHECK_EQ(13, n);
            TF_CHECK_EQ(0x55, tx[0]);
            TF_CHECK_MEM(rom, &tx[1], 8);
            TF_CHECK_EQ(0x55, tx[9]);
            TF_CHECK_EQ(0x20, tx[10]);
            TF_CHECK_EQ(0x00, tx[11]);
            TF_CHECK_EQ(0x07, tx[12]);
        }
    }

    /* Scratchpad echo mismatch -> no copy, error. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);

        memset(data, 0x55, sizeof(data));
        dev->echo_bad = true;
        ds2431_init(&d);
        TF_CHECK_EQ(OW_CRC_ERROR, run_write(&d, 0, 0u, data));
        TF_CHECK_EQ(0, dev->mem[0]);
        TF_CHECK(!mock_strong());
    }

    /* Partial-full flag set -> rejected before Copy. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);

        memset(data, 0x5A, sizeof(data));
        dev->force_pf = true;
        ds2431_init(&d);
        TF_CHECK_EQ(OW_CRC_ERROR, run_write(&d, 0, 0u, data));
        TF_CHECK_EQ(0, dev->mem[0]);
    }

    /* Corrupted scratchpad CRC -> rejected. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);

        memset(data, 0x77, sizeof(data));
        dev->sp_frame_crc_bad = true;
        ds2431_init(&d);
        TF_CHECK_EQ(OW_CRC_ERROR, run_write(&d, 0, 0u, data));
        TF_CHECK_EQ(0, dev->mem[0]);
    }

    /* Copy refused by the device (status != 0xAA) -> bus error. */
    mock_reset();
    {
        mock_dev_t *dev = mock_add(MOCK_DS2431, rom);

        memset(data, 0x11, sizeof(data));
        dev->force_status_bad = true;
        ds2431_init(&d);
        TF_CHECK_EQ(OW_BUS_ERROR, run_write(&d, 0, 8u, data));
        TF_CHECK_EQ(0x11, dev->mem[8]);
        TF_CHECK_EQ(0x00, d.status);
    }

    /* Absent device. */
    mock_reset();
    ds2431_init(&d);
    TF_CHECK_EQ(OW_NO_PRESENCE, run_read(&d, 0, 0u, buf, 4u));
    /* The failure is sticky, not replayed as OW_OK. */
    TF_CHECK_EQ(OW_NO_PRESENCE, ds2431_read_poll(&d));
    TF_CHECK_EQ(OW_NO_PRESENCE, run_write(&d, 0, 0u, data));
    TF_CHECK_EQ(OW_NO_PRESENCE, ds2431_write_poll(&d));

    /* Parameter guards. */
    mock_reset();
    (void)mock_add(MOCK_DS2431, rom);
    ds2431_init(&d);
    TF_CHECK(!ds2431_read_start(0, 0, 0u, buf, 1u));
    TF_CHECK(!ds2431_read_start(&d, 0, 0u, 0, 1u));
    TF_CHECK(!ds2431_read_start(&d, 0, 0u, buf, OW_MAX_BYTES + 1u));
    TF_CHECK(!ds2431_read_start(&d, 0, 1020u, buf, 8u));
    TF_CHECK(!ds2431_write_start(0, 0, 0u, data));
    TF_CHECK(!ds2431_write_start(&d, 0, 0u, 0));
    TF_CHECK(!ds2431_write_start(&d, 0, 4u, data));
    TF_CHECK(!ds2431_write_start(&d, 0, 1020u, data));
    TF_CHECK_EQ(OW_PARAM, ds2431_read_poll(0));
    TF_CHECK_EQ(OW_PARAM, ds2431_write_poll(0));

    /* A start while the bus is active is rejected. */
    mock_set_active(true);
    TF_CHECK(!ds2431_read_start(&d, 0, 0u, buf, 1u));
    TF_CHECK(!ds2431_write_start(&d, 0, 0u, data));
    mock_set_active(false);

    /* Second start while a write is in flight is rejected. */
    {
        int before;

        memset(data, 0x42, sizeof(data));
        TF_CHECK(ds2431_write_start(&d, 0, 0x20u, data));
        TF_CHECK_EQ(OW_BUSY, ds2431_write_poll(&d));
        before = mock_event_count();
        TF_CHECK(!ds2431_write_start(&d, 0, 0x28u, data));
        TF_CHECK_EQ(before, mock_event_count());
        {
            int guard = 400;
            ow_status_t st;

            while (((st = ds2431_write_poll(&d)) == OW_BUSY) && (guard-- > 0)) {
            }
            TF_CHECK_EQ(OW_OK, st);
        }
        TF_CHECK_EQ(0x42, mock_dev(0)->mem[0x20]);
    }

    /* Round-trip every 8-byte block with deterministic data. */
    {
        uint32_t lcg = 0xC0FFEEu;
        int block;
        int failures = 0;

        mock_reset();
        (void)mock_add(MOCK_DS2431, rom);
        ds2431_init(&d);

        for (block = 0; block < 128; block++) {
            uint16_t addr = (uint16_t)(block * 8);
            int i;

            for (i = 0; i < 8; i++) {
                lcg = (lcg * 1664525u) + 1013904223u;
                data[i] = (uint8_t)(lcg >> 13);
            }
            if (run_write(&d, 0, addr, data) != OW_OK) {
                failures++;
                continue;
            }
            if (run_read(&d, 0, addr, buf, 8u) != OW_OK) {
                failures++;
                continue;
            }
            if (memcmp(buf, data, 8u) != 0) {
                failures++;
            }
        }
        TF_CHECK_EQ(0, failures);
    }
}
