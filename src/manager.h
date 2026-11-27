#ifndef MANAGER_H
#define MANAGER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register-map constants shared with the I2C bridge in app/main.c.
 */
#define REG_MAGIC               0x00u
#define REG_VERSION             0x01u
#define BRIDGE_MAP_VERSION      0x05u
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
#define REG_STALL               0x66u   /* publish phase 0 max, us */
#define REG_STALL_STATE         0x67u   /* publish phase 1 max, us */
#define REG_STALL_P2            0x68u   /* publish phase 2 max, us */
#define REG_STALL_AVG           0x69u   /* publish phase 0/1/2 averages, us */
#define REG_DIAG_CRC_FFFF       0x6Cu   /* long reads stopped before byte 250 */
#define REG_DIAG_BUSY           0x6Du   /* tx_index of the last early STOP */
#define REG_DIAG_ACK            0x6Eu   /* reads decoded as writes (>8 bytes) */
#define REG_DIAG_REASON         0x6Fu   /* transmit underruns (OUF) */
#define REG_DIAG_GATE           0x49u   /* STOPF refused mid-transmit (live) */
#define REG_DIAG_GATE_POS       0x4Au   /* tx_index at the last refusal (live) */
#define REG_DIAG_START          0x4Cu   /* STARTF while a frame was open */
#define REG_DIAG_BUS            0x4Du   /* BUSERR while a frame was open */
#define REG_DIAG_EARLY_NACK     0x4Eu   /* ACKFAIL before the last map byte */
#define REG_DIAG_EARLY_NACK_POS 0x4Fu   /* tx_index of the last early NACK */
#define REG_MAX_STEP            0x79u   /* worst manager_poll, us, 16-bit LE */
#define REG_MAX_STEP_H          0x7Au
#define REG_MAX_STATE           0x7Bu   /* manager state of that call */
#define REG_OUF_POS             0x7Cu   /* tx_index when an underrun hit */
#define REG_OUF_PTR             0x7Du   /* reg_ptr at that moment */
#define REG_EEP_START           0x7Eu   /* worst ds2431_read_start, us */
#define REG_EEP_POLL            0x7Fu   /* worst ds2431_read_poll, us */
#define REG_SCRATCH             0x70u
#define REG_ROMS                0x80u

#define FLAG_SEARCH_DONE        0x01u
#define FLAG_SEARCH_ERROR       0x02u
#define FLAG_BUSY               0x04u
#define FLAG_SNAPSHOT           0x08u
#define FLAG_EEPROM_OK          0x10u
#define FLAG_TEMP_ERROR         0x20u

/* REG_HEALTH sticky bits. b0..b2 come from I2C status flags. b3/b4 are
 * production-only recovery indicators; DIAG_BUILD leaves them clear. */
#define HEALTH_BUSERR           0x01u
#define HEALTH_ARLOST           0x02u
#define HEALTH_OUF              0x04u
#define HEALTH_SDA_STUCK        0x08u   /* production: SDA stuck recovery */
#define HEALTH_RECOVERED        0x10u   /* production: recovery completed */
#define HEALTH_RECOVERY_FAILED  0x20u   /* production: SCL/SDA not released */

/* REG_FAULT codes: one shot, applied to the next transmitted byte. */
#define FAULT_BIT_FLIP          1u
#define FAULT_STRETCH           2u      /* reserved: not reliable on this slave */
#define FAULT_SKIP_BYTE         3u
#define FAULT_EXTRA_FRAME       4u
#define FAULT_DROP_FRAME        5u
#define FAULT_CURRENT           6u      /* strong pull-up during the transfer */
#define FAULT_EDGES             7u      /* 1-Wire burst during the transfer */
#define FAULT_STALL             8u      /* CPU stall in the middle of a read */

#define DEV_MAX                 16u
_Static_assert(REG_ROMS + 8u * DEV_MAX <= 256u, "ROM map overflows the register file");

enum {
    MGR_SEARCH = 0,
    MGR_CONVERT,
    MGR_READ,
    MGR_EEPROM,
    MGR_PUBLISH,
    MGR_WAIT
};

/* Shared register file, defined here but also visible to the I2C bridge
 * in app/i2c_slave.c through this declaration. */
extern uint8_t g_regs[256];

/* Initial state for a cold start. */
void manager_init(void);

/* Advance the 1-Wire manager state machine.  Call from the superloop
 * together with i2c_poll(). */
void manager_poll(void);

/* Called by the I2C bridge when a command is latched at STOP. */
void manager_command(uint8_t cmd);

/* Manager state, injected into REG_MGR at read time (diagnostics). */
uint8_t manager_state_reg(void);

/* Publish-window diagnostics. */
bool manager_publish_busy(void);
uint8_t manager_crc_ffff_count(void);
const uint8_t *manager_map(void);
void manager_select(uint8_t idx);
uint32_t manager_eep_start_max(void);
uint32_t manager_eep_poll_max(void);

/* True while the I2C bridge is mid-transaction; overrides the weak stub
 * in manager.c on the real target. */
bool manager_i2c_busy(void);

/* Test helper: force the manager state (tests only). */
void manager_set_state_for_test(uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* MANAGER_H */







