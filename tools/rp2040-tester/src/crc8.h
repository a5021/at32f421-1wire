#ifndef TESTER_CRC8_H
#define TESTER_CRC8_H

#include <stddef.h>
#include <stdint.h>

/* Dallas/Maxim CRC-8, polynomial 0x8C, reflected. */
uint8_t crc8_dallas(const uint8_t *data, size_t len);

#endif /* TESTER_CRC8_H */
