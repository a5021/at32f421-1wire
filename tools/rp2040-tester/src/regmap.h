#ifndef TESTER_REGMAP_H
#define TESTER_REGMAP_H

#include <stdint.h>

/* AT32F421 1-Wire bridge, I2C slave 0x42.  Mirrors src/manager.h. */
#define BRIDGE_ADDR             0x42u

#define REG_MAGIC               0x00u
#define REG_VERSION             0x01u
#define REG_FLAGS               0x02u
#define REG_DEVICES             0x03u
#define REG_DS18B20             0x04u
#define REG_ERROR               0x05u
#define REG_CYCLE_L             0x06u
#define REG_CYCLE_H             0x07u
#define REG_SEQ                 0x08u
#define REG_FRAMES              0x09u   /* 0x09..0x0C, 32-bit LE */
#define REG_HEALTH              0x0Du
#define REG_ERR_BUS             0x0Eu
#define REG_SPEED               0x0Fu
#define REG_TEMP                0x10u
#define REG_TEMP_STATUS         0x30u
#define REG_EEPROM              0x40u
#define REG_EEPROM_STATUS       0x48u
#define REG_CMD                 0x60u
#define REG_SELECTED            0x61u
#define REG_SCRATCH             0x70u
#define REG_ROMS                0x80u

#define FLAG_SEARCH_DONE        0x01u
#define FLAG_SEARCH_ERROR       0x02u
#define FLAG_BUSY               0x04u
#define FLAG_SNAPSHOT           0x08u
#define FLAG_EEPROM_OK          0x10u
#define FLAG_TEMP_ERROR         0x20u

#define HEALTH_BUSERR           0x01u
#define HEALTH_ARLOST           0x02u
#define HEALTH_OUF              0x04u
#define HEALTH_SDA_STUCK        0x08u
#define HEALTH_RECOVERED        0x10u

#define DEV_MAX                 16u

#define CMD_RESCAN              1u
#define CMD_REREAD_EEPROM       2u
#define CMD_CLEAR_COUNTERS      3u

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
