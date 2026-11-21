#ifndef TESTER_CRC16_H
#define TESTER_CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout.
 * Mirrors map_crc() in the bridge firmware. */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);

#endif /* TESTER_CRC16_H */
