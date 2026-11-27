#ifndef I2C_HW_MOCK_H
#define I2C_HW_MOCK_H

#include <stdbool.h>
#include <stdint.h>

#define I2C_MOCK_MAX_EVENTS 256u

typedef enum {
    MOCK_EVT_INIT = 1,
    MOCK_EVT_STS1,
    MOCK_EVT_STS2,
    MOCK_EVT_GPIO,
    MOCK_EVT_DT_READ,
    MOCK_EVT_DT_WRITE,
    MOCK_EVT_CLEAR_FLAGS,
    MOCK_EVT_STOP_REARM,
    MOCK_EVT_ABORT,
    MOCK_EVT_APPLY_SPEED,
    MOCK_EVT_RECOVER,
    MOCK_EVT_DELAY_US
} i2c_mock_event_type_t;

typedef struct {
    uint8_t type;
    uint32_t a;
    uint32_t b;
} i2c_mock_event_t;

extern i2c_mock_event_t i2c_mock_events[I2C_MOCK_MAX_EVENTS];
extern uint32_t i2c_mock_event_count;
extern uint32_t i2c_mock_cycles;
extern uint32_t i2c_mock_speed_hz;
extern uint32_t i2c_mock_sts1;
extern uint32_t i2c_mock_sts2;
extern uint32_t i2c_mock_gpio;
extern uint8_t i2c_mock_dt;
extern bool i2c_mock_recover_result;

void i2c_hw_mock_reset(void);
void i2c_hw_mock_set_status(uint32_t sts1, uint32_t sts2, uint32_t gpio);
void i2c_hw_mock_set_dt(uint8_t value);
void i2c_hw_mock_set_cycles(uint32_t cycles);
void i2c_hw_mock_set_recover_result(bool result);
uint32_t i2c_hw_mock_count(uint8_t type);
i2c_mock_event_t i2c_hw_mock_event(uint32_t index);

#endif /* I2C_HW_MOCK_H */
