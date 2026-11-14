#ifndef DS2431_H
#define DS2431_H

#include <stdint.h>
#include <stdbool.h>
#include "onewire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS2431_EEPROM_SIZE      1024u
#define DS2431_BLOCK_SIZE       8u
#define DS2431_SCRATCH_SIZE     8u
#define DS2431_TPROG_US         11000u

/*
 * Non-blocking DS2431 driver.
 *
 * Read Memory (0xF0) has no CRC: the driver returns raw bytes and only checks
 * that the bus behaved (captures present).
 *
 * Writes are restricted to one aligned 8-byte EEPROM block per call.  The
 * sequence is Write Scratchpad (0x0F) -> Read Scratchpad (0xAA, verify echo,
 * PF and inverted CRC-16) -> Copy Scratchpad (0x55, tPROG) -> status 0xAA.
 *
 * Buffers passed to _start must stay valid until _poll reports completion.
 */

typedef enum {
    DS2431_PH_IDLE = 0,
    DS2431_PH_READ,
    DS2431_PH_WRITE
} ds2431_mode_t;

typedef struct {
    /* public: last byte read after Copy Scratchpad */
    uint8_t  status;

    /* private */
    uint8_t  phase;
    uint8_t  mode;
    uint8_t  len;
    uint8_t  es;
    bool     match_rom;
    uint8_t  rom[8];
    uint16_t addr;
    uint8_t *dst;
    const uint8_t *src;
    uint8_t  sp[13];
} ds2431_t;

void ds2431_init(ds2431_t *d);

/* Read 1..OW_MAX_BYTES bytes starting at addr.  rom == 0 selects Skip ROM,
 * otherwise Match ROM is used. */
bool ds2431_read_start(ds2431_t *d, const uint8_t *rom, uint16_t addr,
                       uint8_t *dst, uint8_t len);
ow_status_t ds2431_read_poll(ds2431_t *d);

/* Write exactly 8 bytes to an 8-byte aligned address. */
bool ds2431_write_start(ds2431_t *d, const uint8_t *rom, uint16_t addr,
                        const uint8_t *src);
ow_status_t ds2431_write_poll(ds2431_t *d);

#ifdef __cplusplus
}
#endif

#endif /* DS2431_H */
