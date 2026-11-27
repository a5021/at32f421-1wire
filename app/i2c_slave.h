#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t g_i2c_speed;
extern volatile uint32_t g_i2c_err_bus;
extern volatile uint32_t g_i2c_err_arl;
extern volatile uint32_t g_i2c_err_ouf;

void i2c_slave_init(void);
void i2c_slave_set_cycles_per_us(uint32_t cycles);
bool i2c_slave_busy(void);
void i2c_slave_poll(void);
void i2c_slave_health_poll(uint32_t now_ms);
bool i2c_slave_recovery_due(uint32_t now_ms);
void i2c_slave_recover(uint32_t now_ms);
void i2c_slave_note_manager_poll(uint32_t cycles, uint8_t state);
uint8_t i2c_slave_reg_byte(uint32_t addr);

#endif /* I2C_SLAVE_H */
