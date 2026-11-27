#ifndef I2C_HW_H
#define I2C_HW_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    I2C_RECOVERY_DEFERRED = 0,
    I2C_RECOVERY_OK,
    I2C_RECOVERY_FAILED
} i2c_recovery_result_t;

void i2c_hw_init(uint32_t scl_hz);
uint32_t i2c_hw_sts1(void);
uint32_t i2c_hw_sts2(void);
uint32_t i2c_hw_gpio_idt(void);
uint8_t i2c_hw_dt_read(void);
void i2c_hw_dt_write(uint8_t value);
void i2c_hw_clear_flags(uint32_t flags);
void i2c_hw_stop_rearm(void);
void i2c_hw_abort(void);
void i2c_hw_apply_speed(uint32_t scl_hz);
bool i2c_hw_busy(void);
i2c_recovery_result_t i2c_hw_recover(uint32_t scl_hz, bool allow_low_scl);
void i2c_hw_delay_us(uint32_t us);
uint32_t i2c_hw_cycles(void);

#endif /* I2C_HW_H */
