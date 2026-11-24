#ifndef DS18B20_H
#define DS18B20_H

#include <stdint.h>
#include <stdbool.h>
#include "onewire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS18B20_CONV_US_9       93750u
#define DS18B20_CONV_US_10      187500u
#define DS18B20_CONV_US_11      375000u
#define DS18B20_CONV_US_12      750000u

typedef struct {
    uint8_t  rom[8];
    bool     match_rom;
    bool     parasite;
    uint32_t conv_us;

    /* private */
    uint8_t  phase;
    uint8_t  stage;
    uint8_t  sp[9];
    uint8_t  cmd[10];               /* command packet, expanded in chunks */
    uint8_t  cmd_len;
    int16_t  raw;
} ds18b20_t;

void        ds18b20_init(ds18b20_t *d, bool parasite);

/* Convert T (0x44).  With parasitic power the strong pull-up is engaged
 * before the command is sent and released when the conversion time expires.
 * Returns false when the part is still busy, so the caller does not mistake a
 * refused start for a finished conversion. */
bool        ds18b20_convert_start(ds18b20_t *d, const uint8_t *rom);
ow_status_t ds18b20_convert_poll(ds18b20_t *d);

/* Read Scratchpad (0xBE), 9 bytes, CRC-8 checked. */
bool        ds18b20_read_start(ds18b20_t *d, const uint8_t *rom);
ow_status_t ds18b20_read_poll(ds18b20_t *d);

/* convert -> wait -> read in one state machine. */
void        ds18b20_temp_start(ds18b20_t *d, const uint8_t *rom);
ow_status_t ds18b20_temp_poll(ds18b20_t *d, int16_t *raw);

bool        ds18b20_crc_ok(const ds18b20_t *d);
float       ds18b20_celsius(const ds18b20_t *d);

#ifdef __cplusplus
}
#endif

#endif /* DS18B20_H */
