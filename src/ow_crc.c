#include "ow_crc.h"

uint8_t ow_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0u;

    for (uint8_t i = 0u; i < len; i++) {
        uint8_t in = data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            uint8_t mix = (uint8_t)((crc ^ in) & 1u);
            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0x8Cu;
            }
            in >>= 1;
        }
    }
    return crc;
}

uint16_t ow_crc16(const uint8_t *data, uint8_t len)
{
    uint16_t crc = 0u;

    for (uint8_t i = 0u; i < len; i++) {
        uint16_t in = data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            uint16_t mix = (uint16_t)((crc ^ in) & 1u);
            crc >>= 1;
            if (mix != 0u) {
                crc ^= 0xA001u;
            }
            in >>= 1;
        }
    }
    return crc;
}
