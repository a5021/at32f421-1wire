#include "test.h"
#include "ow_hal_mock.h"
#include "onewire.h"

static ow_status_t run_reset(bool *presence)
{
    int guard = 200;
    ow_status_t st;

    *presence = false;
    ow_reset_start();
    while (((st = ow_reset_poll(presence)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

static ow_status_t run_read(uint8_t *dst, uint16_t len)
{
    int guard = 200;
    ow_status_t st;

    if (!ow_read_start(len)) {
        return OW_PARAM;
    }
    while (((st = ow_read_poll(dst)) == OW_BUSY) && (guard-- > 0)) {
    }
    return st;
}

void test_bus_run(void)
{
    static const uint8_t rom[8] = { 0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x00 };
    bool presence;

    /* Presence detection with one device. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    TF_CHECK_EQ(OW_OK, run_reset(&presence));
    TF_CHECK(presence);

    /* Empty bus -> no presence. */
    mock_reset();
    TF_CHECK_EQ(OW_OK, run_reset(&presence));
    TF_CHECK(!presence);

    /* Captured edges below the presence threshold -> no presence. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_set_reset_captures(0);
    TF_CHECK_EQ(OW_OK, run_reset(&presence));
    TF_CHECK(!presence);
    mock_set_reset_captures(2);

    /* Completion needs the programmed number of polls when busy is injected. */
    mock_reset();
    (void)mock_add(MOCK_DS18B20, rom);
    mock_set_busy_ticks(3);
    {
        int busy = 0;
        int i;

        ow_reset_start();
        presence = false;
        for (i = 0; i < 10; i++) {
            ow_status_t st = ow_reset_poll(&presence);
            if (st == OW_BUSY) {
                busy++;
            } else {
                TF_CHECK_EQ(OW_OK, st);
                break;
            }
        }
        TF_CHECK_EQ(3, busy);
        TF_CHECK(presence);
    }

    /* Read path: all-zero, all-one and mixed byte patterns. */
    {
        static const uint8_t in0[1] = { 0x00 };
        static const uint8_t in1[1] = { 0xFF };
        static const uint8_t in2[2] = { 0xA5, 0x3C };
        uint8_t out[2];

        mock_reset();
        mock_parser(false);

        mock_queue_read(in0, 1);
        out[0] = 0xAA;
        TF_CHECK_EQ(OW_OK, run_read(out, 1u));
        TF_CHECK_EQ(0x00, out[0]);

        mock_queue_read(in1, 1);
        out[0] = 0xAA;
        TF_CHECK_EQ(OW_OK, run_read(out, 1u));
        TF_CHECK_EQ(0xFF, out[0]);

        mock_queue_read(in2, 2);
        out[0] = out[1] = 0;
        TF_CHECK_EQ(OW_OK, run_read(out, 2u));
        TF_CHECK_MEM(in2, out, 2);
    }

    /* Sampling threshold: <= OW_T_SAMPLE_MAX is a logic 1. */
    {
        static const uint8_t caps[8] = {
            5, 10, 11, 60, 0, 255, 7, 9
        };
        uint8_t out = 0;

        mock_reset();
        mock_parser(false);
        mock_queue_captures(caps, 8);
        TF_CHECK_EQ(OW_OK, run_read(&out, 1u));
        TF_CHECK_EQ(0xD3, out);
    }

    /* A short capture stream is reported as a bus error. */
    {
        uint8_t out = 0;

        mock_reset();
        mock_parser(false);
        mock_queue_read((const uint8_t *)"\x00", 1);
        mock_set_read_shortfall(1);
        TF_CHECK_EQ(OW_BUS_ERROR, run_read(&out, 1u));
    }

    /* NULL destination, empty and oversized requests. */
    {
        mock_reset();
        mock_parser(false);
        TF_CHECK(ow_read_start(1u));
        TF_CHECK_EQ(OW_BUS_ERROR, ow_read_poll(0));
        (void)ow_hal_done();

        TF_CHECK(!ow_read_start(0u));
        TF_CHECK(!ow_read_start(OW_MAX_BYTES + 1u));
    }

    /* A full-size 32-byte read. */
    {
        uint8_t dst[OW_MAX_BYTES];
        uint8_t src[OW_MAX_BYTES];

        mock_reset();
        mock_parser(false);
        memset(src, 0x5A, sizeof(src));
        mock_queue_read(src, (int)sizeof(src));
        TF_CHECK_EQ(OW_OK, run_read(dst, OW_MAX_BYTES));
        TF_CHECK_MEM(src, dst, OW_MAX_BYTES);
    }
}
