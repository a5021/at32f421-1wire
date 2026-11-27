#include <stdint.h>

#include "i2c_hw.h"
#include "i2c_hw_mock.h"

#define MOCK_ADDRF  0x00000002u
#define MOCK_STOPF  0x00000010u
#define MOCK_BUSYF  0x00000002u

i2c_mock_event_t i2c_mock_events[I2C_MOCK_MAX_EVENTS];
uint32_t i2c_mock_event_count;
uint32_t i2c_mock_cycles;
uint32_t i2c_mock_speed_hz;
uint32_t i2c_mock_sts1;
uint32_t i2c_mock_sts2;
uint32_t i2c_mock_gpio;
uint8_t i2c_mock_dt;
bool i2c_mock_recover_result;

static void record(uint8_t type, uint32_t a, uint32_t b)
{
    if (i2c_mock_event_count < I2C_MOCK_MAX_EVENTS) {
        i2c_mock_events[i2c_mock_event_count].type = type;
        i2c_mock_events[i2c_mock_event_count].a = a;
        i2c_mock_events[i2c_mock_event_count].b = b;
    }
    i2c_mock_event_count++;
}

void i2c_hw_mock_reset(void)
{
    i2c_mock_event_count = 0u;
    i2c_mock_cycles = 0u;
    i2c_mock_speed_hz = 0u;
    i2c_mock_sts1 = 0u;
    i2c_mock_sts2 = 0u;
    i2c_mock_gpio = 0x00000003u;
    i2c_mock_dt = 0u;
    i2c_mock_recover_result = true;
}

void i2c_hw_mock_set_status(uint32_t sts1, uint32_t sts2, uint32_t gpio)
{
    i2c_mock_sts1 = sts1;
    i2c_mock_sts2 = sts2;
    i2c_mock_gpio = gpio;
}

void i2c_hw_mock_set_dt(uint8_t value)
{
    i2c_mock_dt = value;
}

void i2c_hw_mock_set_cycles(uint32_t cycles)
{
    i2c_mock_cycles = cycles;
}

void i2c_hw_mock_set_recover_result(bool result)
{
    i2c_mock_recover_result = result;
}

uint32_t i2c_hw_mock_count(uint8_t type)
{
    uint32_t count = 0u;
    uint32_t limit = i2c_mock_event_count;

    if (limit > I2C_MOCK_MAX_EVENTS) {
        limit = I2C_MOCK_MAX_EVENTS;
    }
    for (uint32_t i = 0u; i < limit; i++) {
        if (i2c_mock_events[i].type == type) {
            count++;
        }
    }
    return count;
}

i2c_mock_event_t i2c_hw_mock_event(uint32_t index)
{
    i2c_mock_event_t empty = {0u, 0u, 0u};

    return (index < I2C_MOCK_MAX_EVENTS) ? i2c_mock_events[index] : empty;
}

void i2c_hw_init(uint32_t scl_hz)
{
    record(MOCK_EVT_INIT, scl_hz, 0u);
    i2c_mock_speed_hz = scl_hz;
    i2c_mock_sts1 = 0u;
    i2c_mock_sts2 = 0u;
}

uint32_t i2c_hw_sts1(void)
{
    record(MOCK_EVT_STS1, i2c_mock_sts1, 0u);
    return i2c_mock_sts1;
}

uint32_t i2c_hw_sts2(void)
{
    record(MOCK_EVT_STS2, i2c_mock_sts2, 0u);
    i2c_mock_sts1 &= ~(MOCK_ADDRF | MOCK_STOPF);
    return i2c_mock_sts2;
}

uint32_t i2c_hw_gpio_idt(void)
{
    record(MOCK_EVT_GPIO, i2c_mock_gpio, 0u);
    return i2c_mock_gpio;
}

uint8_t i2c_hw_dt_read(void)
{
    record(MOCK_EVT_DT_READ, i2c_mock_dt, 0u);
    return i2c_mock_dt;
}

void i2c_hw_dt_write(uint8_t value)
{
    record(MOCK_EVT_DT_WRITE, value, 0u);
    i2c_mock_dt = value;
}

void i2c_hw_clear_flags(uint32_t flags)
{
    record(MOCK_EVT_CLEAR_FLAGS, flags, 0u);
    i2c_mock_sts1 &= ~(flags & 0x0000DF00u);
}

void i2c_hw_stop_rearm(void)
{
    record(MOCK_EVT_STOP_REARM, 0u, 0u);
}

void i2c_hw_abort(void)
{
    record(MOCK_EVT_ABORT, 0u, 0u);
    i2c_mock_dt = 0xFFu;
    i2c_mock_sts1 = 0u;
}

void i2c_hw_apply_speed(uint32_t scl_hz)
{
    record(MOCK_EVT_APPLY_SPEED, scl_hz, 0u);
    i2c_mock_speed_hz = scl_hz;
}

bool i2c_hw_busy(void)
{
    return (i2c_mock_sts2 & MOCK_BUSYF) != 0u;
}

bool i2c_hw_recover(uint32_t scl_hz)
{
    record(MOCK_EVT_RECOVER, scl_hz, 0u);
    i2c_mock_sts1 = 0u;
    i2c_mock_sts2 = 0u;
    if (i2c_mock_recover_result) {
        i2c_mock_gpio = 0x00000003u;
    }
    return i2c_mock_recover_result;
}

void i2c_hw_delay_us(uint32_t us)
{
    record(MOCK_EVT_DELAY_US, us, 0u);
}

uint32_t i2c_hw_cycles(void)
{
    return i2c_mock_cycles;
}
