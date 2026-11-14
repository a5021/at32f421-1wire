#include "test.h"
#include "ow_hal_mock.h"
#include "onewire.h"

static const uint8_t *finish_write(void)
{
    int guard = 16;

    while ((ow_write_poll() == OW_BUSY) && (guard-- > 0)) {
    }
    return mock_last_pulses();
}

void test_encode_run(void)
{
    uint8_t data[8];
    const uint8_t *pulses;
    int i;

    mock_reset();
    mock_parser(false);

    /* 0x00 -> all long lows. */
    data[0] = 0x00;
    TF_CHECK(ow_write_start(data, 1u));
    pulses = finish_write();
    TF_CHECK_EQ(8, mock_last_feed_slots());
    for (i = 0; i < 8; i++) {
        TF_CHECK_EQ(OW_T_LOW0, pulses[i]);
    }
    TF_CHECK_EQ(0, pulses[8]);

    /* 0xFF -> all short lows. */
    mock_reset();
    mock_parser(false);
    data[0] = 0xFF;
    TF_CHECK(ow_write_start(data, 1u));
    pulses = finish_write();
    for (i = 0; i < 8; i++) {
        TF_CHECK_EQ(OW_T_LOW1, pulses[i]);
    }

    /* 0xA5 -> 1 0 1 0 0 1 0 1 (LSB first). */
    mock_reset();
    mock_parser(false);
    data[0] = 0xA5;
    TF_CHECK(ow_write_start(data, 1u));
    pulses = finish_write();
    {
        static const uint8_t expect[8] = {
            OW_T_LOW1, OW_T_LOW0, OW_T_LOW1, OW_T_LOW0,
            OW_T_LOW0, OW_T_LOW1, OW_T_LOW0, OW_T_LOW1
        };
        TF_CHECK_MEM(expect, pulses, 8);
    }

    /* Multi-byte frame: pulses are grouped per byte, trailing release is 0. */
    mock_reset();
    mock_parser(false);
    data[0] = 0x00;
    data[1] = 0xFF;
    data[2] = 0x0F;
    TF_CHECK(ow_write_start(data, 3u));
    pulses = finish_write();
    TF_CHECK_EQ(24, mock_last_feed_slots());
    for (i = 0; i < 8; i++) {
        TF_CHECK_EQ(OW_T_LOW0, pulses[i]);
        TF_CHECK_EQ(OW_T_LOW1, pulses[8 + i]);
    }
    TF_CHECK_EQ(0, pulses[24]);

    /* The mock decodes the exact bytes back from the pulse durations. */
    {
        int n = 0;
        const uint8_t *tx = mock_last_tx(&n);
        TF_CHECK_EQ(3, n);
        TF_CHECK_MEM(data, tx, 3);
    }

    /* Random encode/decode property test. */
    {
        uint32_t lcg = 0x1234567u;
        int iter;

        mock_reset();
        mock_parser(false);
        for (iter = 0; iter < 100; iter++) {
            int n = 0;
            const uint8_t *tx;

            for (i = 0; i < 8; i++) {
                lcg = (lcg * 1103515245u) + 12345u;
                data[i] = (uint8_t)(lcg >> 16);
            }
            mock_reset();
            mock_parser(false);
            if (!ow_write_start(data, 8u)) {
                TF_CHECK(false);
                break;
            }
            (void)finish_write();
            tx = mock_last_tx(&n);
            if ((n != 8) || (memcmp(tx, data, 8u) != 0)) {
                TF_CHECK(false);
                break;
            }
        }
        TF_CHECK(true);
    }

    /* Parameter guards. */
    mock_reset();
    mock_parser(false);
    TF_CHECK(!ow_write_start(0, 1u));
    TF_CHECK(!ow_write_start(data, 0u));
    TF_CHECK(!ow_write_start(data, OW_MAX_BYTES + 1u));
    TF_CHECK(ow_write_start(data, OW_MAX_BYTES));

    /* A busy engine rejects a new frame. */
    mock_set_active(true);
    TF_CHECK(!ow_write_start(data, 1u));
    mock_set_active(false);
    TF_CHECK_EQ(OW_OK, ow_write_poll());
}
