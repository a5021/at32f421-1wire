#include "test.h"
#include "ow_crc.h"

static const uint8_t text[] = "123456789";

void test_crc_run(void)
{
    /* Standard Dallas/Maxim check vector. */
    TF_CHECK_EQ(0xA1, ow_crc8(text, 9u));
    TF_CHECK_EQ(0xBB3D, ow_crc16(text, 9u));
    TF_CHECK_EQ(0x44C2, (uint16_t)~ow_crc16(text, 9u));

    /* Empty input leaves both registers at zero. */
    TF_CHECK_EQ(0, ow_crc8(text, 0u));
    TF_CHECK_EQ(0, ow_crc16(text, 0u));

    /* Known DS18B20 ROM (family 0x28, CRC over 7 bytes). */
    {
        static const uint8_t rom[8] = {
            0x28, 0x7F, 0x1E, 0x5B, 0x16, 0x03, 0x00, 0xFE
        };
        TF_CHECK_EQ(0xFE, ow_crc8(rom, 7u));
        TF_CHECK_EQ(0, ow_crc8(rom, 8u));
    }

    /* DS2431 scratchpad frame: 0xAA + TA1 + TA2 + E/S + 8 data. */
    {
        static const uint8_t frame[12] = {
            0xAA, 0x00, 0x00, 0x07, 0x00, 0x01, 0x02, 0x03,
            0x04, 0x05, 0x06, 0x07
        };
        TF_CHECK_EQ(0x6744, (uint16_t)~ow_crc16(frame, 12u));
    }

    /* CRC-16 complement: feeding the CRC low/high back yields zero residue? */
    {
        uint8_t buf[12] = {
            0xAA, 0x00, 0x00, 0x07, 0x00, 0x01, 0x02, 0x03,
            0x04, 0x05, 0x06, 0x07
        };
        uint16_t crc = (uint16_t)~ow_crc16(buf, 12u);
        TF_CHECK_EQ(0x44, (uint8_t)(crc & 0xFFu));
        TF_CHECK_EQ(0x67, (uint8_t)(crc >> 8));
    }
}
