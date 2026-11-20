#include "crc8.h"

uint8_t crc8_dallas(const uint8_t *data, size_t len)
{
    uint8_t crc = 0u;

    while (len-- != 0u) {
        uint8_t b = *data++;

        for (unsigned i = 0u; i < 8u; i++) {
            uint8_t mix = (uint8_t)((crc ^ b) & 1u);

            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0x8Cu;
            }
            b >>= 1;
        }
    }
    return crc;
}
