#ifndef TESTER_REGMAP_H
#define TESTER_REGMAP_H

#include <stdint.h>
#include "manager.h"

/* AT32F421 1-Wire bridge, I2C slave 0x42. Register and flag definitions come
 * from the firmware header so the two sides cannot drift. */
#define BRIDGE_ADDR             0x42u

#define CMD_RESCAN              1u
#define CMD_REREAD_EEPROM       2u
#define CMD_CLEAR_COUNTERS      3u
#define CMD_PAUSE               4u
#define CMD_RESUME              5u
#define CMD_STALL_RESET         7u

#define BRIDGE_VERSION          3u

#define FAMILY_DS18B20          0x28u
#define FAMILY_DS2431           0x2Du
#define FAMILY_DS2413           0x3Au

/* ow_status_t values published in REG_TEMP_STATUS / REG_EEPROM_STATUS. */
#define ST_OK                   0u
#define ST_BUSY                 1u
#define ST_NO_PRESENCE          2u
#define ST_CRC_ERROR            3u
#define ST_PARAM                4u
#define ST_BUS_ERROR            5u
#define ST_END                  6u
#define ST_INVALID              0xFFu

#define INVALID_TEMP            0x8000

#endif /* TESTER_REGMAP_H */
