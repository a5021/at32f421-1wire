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
#define REG_FAULT               0x62u
#define REG_MAP_CRC_L           0x63u
#define REG_MAP_CRC_H           0x64u
#define REG_MGR                 0x65u
#define REG_STALL               0x66u
#define REG_STALL_STATE         0x67u
#define REG_STALL_P2            0x68u
#define REG_STALL_AVG           0x69u
#define REG_DIAG_CRC_FFFF       0x6Cu
#define REG_DIAG_BUSY           0x6Du
#define REG_DIAG_ACK            0x6Eu
#define REG_DIAG_REASON         0x6Fu
/* Underrun forensics (temporary). */
#define REG_OUF_END             0x4Au
#define REG_OUF_MID             0x4Bu
#define REG_ISR_MAX             0x4Cu   /* worst I2C service duration, us */
#define REG_OUF_STS1            0x4Du
#define REG_OUF_STS2            0x4Eu
#define REG_OUF_AGE             0x4Fu
#define REG_ADDR_DT             0x50u   /* ADDR release to first DT write, us */
#define REG_OUF_REG_PTR         0x51u
#define REG_SLOW_COUNT          0x52u
#define REG_SLOW_US             0x53u
#define REG_SLOW_STATE          0x54u
#define REG_SLOW_SUB            0x55u
#define REG_MAX_SUB             0x56u
#define REG_ACT_PREV            0x57u
#define REG_ACT_DT              0x58u
#define REG_ACT_PREV2           0x59u
#define REG_ACT_DT2             0x5Au
#define REG_ADDR_STALE          0x5Bu
#define REG_POLL_GAP_L         0x5Cu   /* worst gap between i2c_poll calls, us (LE) */
#define REG_POLL_GAP_H         0x5Du
#define REG_POLL_GAP_STATE     0x5Eu   /* manager state during that gap */
#define REG_POLL_GAP_SUB       0x5Fu   /* manager subphase during that gap */
#define REG_DT_FULL             0x49u
#define REG_MAX_STEP            0x79u
#define REG_MAX_STEP_H          0x7Au
#define REG_MAX_STATE           0x7Bu
#define REG_OUF_POS             0x7Cu
#define REG_OUF_PTR             0x7Du
#define REG_EEP_START           0x7Eu
#define REG_EEP_POLL            0x7Fu
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
#define CMD_PAUSE               4u
#define CMD_RESUME              5u
#define CMD_STALL_RESET         7u

/* REG_FAULT codes, one shot. */
#define FAULT_BIT_FLIP          1u
#define FAULT_STRETCH           2u
#define FAULT_SKIP_BYTE         3u
#define FAULT_EXTRA_FRAME       4u
#define FAULT_DROP_FRAME        5u
#define FAULT_CURRENT           6u
#define FAULT_EDGES             7u
#define FAULT_STALL             8u

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











