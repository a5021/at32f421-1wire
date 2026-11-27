#ifndef I2C_SELECT_LATCH_H
#define I2C_SELECT_LATCH_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t value;
    bool    pending;
} i2c_select_latch_t;

static inline void i2c_select_latch_write(i2c_select_latch_t *latch,
                                          uint8_t value)
{
    latch->value = (uint8_t)(value & 0x0Fu);
    latch->pending = true;
}

static inline bool i2c_select_latch_commit(i2c_select_latch_t *latch,
                                           uint8_t *value)
{
    if (!latch->pending) {
        return false;
    }
    *value = latch->value;
    latch->pending = false;
    return true;
}

static inline void i2c_select_latch_abort(i2c_select_latch_t *latch)
{
    latch->pending = false;
}

#endif /* I2C_SELECT_LATCH_H */
