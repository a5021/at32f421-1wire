#ifndef OW_CRC_H
#define OW_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Dallas/Maxim CRC-8 (poly 0x31 reflected, init 0, no final xor). */
uint8_t ow_crc8(const uint8_t *data, uint8_t len);

/* Maxim CRC-16 (poly 0x8005 reflected, init 0, no final xor).
 * The DS2431 transmits the bitwise complement of this value, low byte first. */
uint16_t ow_crc16(const uint8_t *data, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif /* OW_CRC_H */
