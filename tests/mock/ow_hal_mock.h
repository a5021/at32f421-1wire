#ifndef OW_HAL_MOCK_H
#define OW_HAL_MOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "ow_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_MAX_DEV        8
#define MOCK_RX_MAX         32
#define MOCK_TX_MAX         64
#define MOCK_LOG_MAX        8192
#define MOCK_PULSE_MAX      (OW_MAX_SLOTS + 1)

typedef enum {
    MOCK_DS18B20 = 0,
    MOCK_DS2431  = 1
} mock_dev_type_t;

typedef enum {
    MOCK_EV_RESET = 0,
    MOCK_EV_FEED,
    MOCK_EV_READ,
    MOCK_EV_READ_PAIR,
    MOCK_EV_WAIT,
    MOCK_EV_STRONG
} mock_ev_kind_t;

typedef struct {
    uint8_t  kind;
    uint32_t a;
    uint32_t b;
} mock_event_t;

typedef struct {
    mock_dev_type_t type;
    uint8_t rom[8];

    /* DS18B20 */
    int16_t temp_raw;
    uint8_t sp18[9];
    bool    sp_crc_bad;
    bool    converting;
    bool    force_no_presence;

    /* DS2431 */
    uint8_t mem[1024];
    uint8_t sp31[8];
    uint16_t ta;
    uint16_t latched_ta;
    uint8_t es;
    bool    pf;
    bool    force_pf;
    uint8_t status;
    bool    force_status_bad;
    bool    echo_bad;
    bool    sp_frame_crc_bad;
} mock_dev_t;

/* Full reset: devices removed, counters/log cleared, parser enabled. */
void mock_reset(void);

/* Device population. */
mock_dev_t *mock_add(mock_dev_type_t type, const uint8_t *rom);
void        mock_remove_all(void);
mock_dev_t *mock_dev(int index);
int         mock_dev_count(void);

/* Fault injection. */
void mock_set_presence(bool present);
void mock_set_presence_drop_at_reset(int reset_number);
void mock_set_reset_captures(int captures);
void mock_set_read_shortfall(int slots);
void mock_set_busy_ticks(int polls_per_pass);
void mock_set_active(bool active);
void mock_parser(bool enabled);
void mock_queue_read(const uint8_t *bytes, int len);
void mock_queue_captures(const uint8_t *values, int len);
void mock_clear_flags(void);

/* Observation. */
bool        mock_busy(void);
bool        mock_strong(void);
bool        mock_read_while_strong(void);
bool        mock_last_cmd_strong(void);
int         mock_last_wait_us(void);
int         mock_last_feed_slots(void);
const uint8_t *mock_last_pulses(void);
const uint8_t *mock_last_tx(int *len);
int         mock_event_count(void);
const mock_event_t *mock_events(void);

#ifdef __cplusplus
}
#endif

#endif /* OW_HAL_MOCK_H */
