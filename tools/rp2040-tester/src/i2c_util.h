#ifndef TESTER_I2C_UTIL_H
#define TESTER_I2C_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "regmap.h"

#define BUS_TIMEOUT_US      100000u
#define BUS_PROBE_US        5000u
#define BUS_SPEED_100K      100000u
/* The RP2040 DW_apb_i2c adds about 27 SCL cycles of overhead per period.
 * Requesting 440 kHz puts the measured SCL at 400 kHz. */
#define BUS_SPEED_FAST      440000u
#define BUS_SPEED_400K      BUS_SPEED_FAST

void     bus_init(void);
void     bus_set_speed(uint32_t hz);
uint32_t bus_speed(void);
bool     bus_probe(uint8_t addr);

int      bus_write_reg(uint8_t reg, const uint8_t *data, size_t len);
int      bus_write_reg_nostop(uint8_t reg, const uint8_t *data, size_t len);
int      bus_read_reg(uint8_t reg, uint8_t *buf, size_t len);
int      bus_read_reg_nostop(uint8_t reg, uint8_t *buf, size_t len);
int      bus_read_only(uint8_t *buf, size_t len);

uint16_t map_cycle(const uint8_t *map);
bool     map_read(uint8_t *map);
bool     map_stable(uint8_t *map, uint32_t timeout_ms);
bool     map_wait_cycle(uint16_t old_cycle, uint8_t *map, uint32_t timeout_ms);
bool     map_wait_not_busy(uint32_t timeout_ms);
bool     map_wait_busy(uint32_t timeout_ms);
bool     map_wait_snapshot(uint8_t *map, uint32_t timeout_ms);

bool     write_speed(uint8_t value);
bool     send_cmd(uint8_t cmd);
bool     select_sensor(uint8_t index);
bool     read_scratch(uint8_t *dst9);

/* One-shot slave fault injection through REG_FAULT. */
bool     bus_fault(uint8_t code);

/* Bit-banged fault injection on the same pins.  The address phase is sent with
 * one address bit inverted, or a write frame is cut short after `data_bits`
 * bits of its data byte.  The bus is returned to the I2C peripheral and
 * recovered before returning. */
bool     bus_bb_corrupt_addr(uint8_t addr7, int flip_bit);
bool     bus_bb_truncate(uint8_t addr7, uint8_t reg, int data_bits);
bool     bus_bb_corrupt_write(uint8_t addr7, uint8_t reg, uint8_t value,
                              int flip_bit);
void     bus_bb_extra_clock(void);

extern volatile uint32_t bus_err_timeout;
extern volatile uint32_t bus_err_nack;
extern volatile uint32_t bus_err_other;
void     bus_stats_reset(void);

/* Bus health accounting.  The slave counts matched addresses; every address
 * phase the master starts is counted here, so the difference is the number of
 * transactions the slave never saw.  That is the only way to observe an
 * address-phase loss, because a NACK on the address leaves no trace on the
 * slave side. */
extern volatile uint32_t bus_txn;
extern volatile uint32_t bus_health_frames;
extern volatile uint32_t bus_health_missed;
extern volatile uint32_t bus_health_truncated;
extern volatile uint8_t  bus_health_flags;
extern volatile uint8_t  bus_health_err_bus;

bool     bus_health_sample(void);
void     bus_health_reset(void);

/* Hold SDA low from the master side for `ms`, then release and recover. */
void     bus_hold_sda_low(uint32_t ms);

/* Release a bus left in the middle of a repeated-start sequence. */
void     bus_recover(void);

#endif /* TESTER_I2C_UTIL_H */
