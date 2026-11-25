#ifndef ONEWIRE_H
#define ONEWIRE_H

#include <stdint.h>
#include <stdbool.h>
#include "ow_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OW_OK = 0,
    OW_BUSY,
    OW_NO_PRESENCE,
    OW_CRC_ERROR,
    OW_PARAM,
    OW_BUS_ERROR,
    OW_END
} ow_status_t;

/*
 * Non-blocking 1-Wire protocol primitives.
 *
 * Every _start function arms exactly one hardware pass (or one search step)
 * and returns immediately.  Call the matching _poll repeatedly until it
 * returns something other than OW_BUSY.  No function blocks.
 */

/* Every _start arms a hardware pass.  The caller must have checked
 * ow_hal_busy() first: a start while a pass is in flight replaces it and the
 * outstanding poll then reports the new pass. */
void        ow_reset_start(void);
ow_status_t ow_reset_poll(bool *presence);

bool        ow_write_start(const uint8_t *data, uint16_t len);
ow_status_t ow_write_poll(void);

/* Chunked variant of ow_write_start for packets too long to expand in one
 * call: a ten-byte packet costs ~13 us of CPU, and the I2C slave has less than
 * that to answer an address.  Build in small steps, then fire. */
void        ow_tx_reset(void);
uint16_t    ow_tx_build(const uint8_t *data, uint16_t len);
bool        ow_tx_fire(uint16_t len);

bool        ow_read_start(uint16_t len);
ow_status_t ow_read_poll(uint8_t *dst);

/* ROM search state.  Zero-initialise or call ow_search_begin() first. */
typedef struct {
    uint8_t rom[8];
    int8_t  last_discrepancy;
    bool    last_device;
    /* private */
    uint8_t phase;
    uint8_t bit;
    uint8_t last_zero;
    uint8_t rom_byte;
    uint8_t rom_mask;
    uint8_t id_bit;
    uint8_t cmp_bit;
    uint8_t dir;
} ow_search_t;

void        ow_search_begin(ow_search_t *s);
ow_status_t ow_search_poll(ow_search_t *s);

/* Hardware-timed delay (uses the same timer). */
void        ow_delay_start(uint32_t us);
bool        ow_delay_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ONEWIRE_H */
