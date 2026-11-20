#include "i2c_util.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define I2C_INST        i2c0
#define PIN_SDA         4u
#define PIN_SCL         5u
#define TX_MAX          264u

static uint8_t  tx_buf[TX_MAX];
static uint32_t current_speed = BUS_SPEED_400K;

volatile uint32_t bus_err_timeout;
volatile uint32_t bus_err_nack;
volatile uint32_t bus_err_other;

void bus_stats_reset(void)
{
    bus_err_timeout = 0u;
    bus_err_nack = 0u;
    bus_err_other = 0u;
}

static void count_err(int ret)
{
    if (ret == PICO_ERROR_TIMEOUT) {
        bus_err_timeout++;
    } else if (ret == PICO_ERROR_GENERIC) {
        bus_err_nack++;
    } else {
        bus_err_other++;
    }
}

void bus_init(void)
{
    i2c_init(I2C_INST, BUS_SPEED_400K);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_SDA);      /* external 2.2k is still required */
    gpio_pull_up(PIN_SCL);
    current_speed = BUS_SPEED_400K;
}

void bus_set_speed(uint32_t hz)
{
    current_speed = i2c_set_baudrate(I2C_INST, (uint)hz);
}

uint32_t bus_speed(void)
{
    return current_speed;
}

void bus_recover(void)
{
    /* Nine clocks plus a STOP release a slave that is holding SDA low after
     * an aborted transfer.  Never drive SCL high while a slave is stretching
     * it low: that is a short, not a recovery. */
    gpio_set_function(PIN_SDA, GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCL, GPIO_FUNC_SIO);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    gpio_set_dir(PIN_SDA, GPIO_IN);
    gpio_set_dir(PIN_SCL, GPIO_IN);

    bool high = false;

    for (int i = 0; i < 1000; i++) {
        if (gpio_get(PIN_SCL)) {
            high = true;
            break;
        }
        busy_wait_us_32(1);
    }
    if (!high) {
        /* The slave still owns SCL; leave the pins alone and just reset the
         * master block. */
        i2c_init(I2C_INST, current_speed);
        gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
        gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
        return;
    }

    gpio_set_dir(PIN_SCL, GPIO_OUT);
    gpio_put(PIN_SCL, 1);
    busy_wait_us_32(5);

    for (int i = 0; i < 9; i++) {
        gpio_put(PIN_SCL, 0);
        busy_wait_us_32(5);
        gpio_put(PIN_SCL, 1);
        busy_wait_us_32(5);
    }

    gpio_put(PIN_SDA, 0);
    gpio_set_dir(PIN_SDA, GPIO_OUT);
    busy_wait_us_32(5);
    gpio_put(PIN_SCL, 1);
    busy_wait_us_32(5);
    gpio_put(PIN_SDA, 1);
    busy_wait_us_32(5);
    gpio_set_dir(PIN_SDA, GPIO_IN);

    i2c_init(I2C_INST, current_speed);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
}

bool bus_probe(uint8_t addr)
{
    uint8_t b = 0u;
    int ret = i2c_read_timeout_us(I2C_INST, addr, &b, 1u, false, BUS_PROBE_US);

    if (ret == PICO_ERROR_TIMEOUT) {
        bus_recover();
    }
    return ret == 1;
}

static bool tx_reg(uint8_t reg, const uint8_t *data, size_t len, bool nostop)
{
    if (len + 1u > TX_MAX) {
        return false;
    }

    tx_buf[0] = reg;
    if (len != 0u) {
        memcpy(&tx_buf[1], data, len);
    }

    int ret = i2c_write_timeout_us(I2C_INST, BRIDGE_ADDR, tx_buf, len + 1u,
                                   nostop, BUS_TIMEOUT_US);
    if (ret != (int)(len + 1u)) {
        count_err(ret);
        if (ret == PICO_ERROR_TIMEOUT) {
            bus_recover();
        }
    }
    return ret == (int)(len + 1u);
}

int bus_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    return tx_reg(reg, data, len, false) ? (int)len : -1;
}

int bus_write_reg_nostop(uint8_t reg, const uint8_t *data, size_t len)
{
    return tx_reg(reg, data, len, true) ? (int)len : -1;
}

int bus_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!tx_reg(reg, NULL, 0u, false)) {
        return -1;
    }
    /* Let the slave service STOPF before the next START. */
    busy_wait_us_32(50);

    int got = i2c_read_timeout_us(I2C_INST, BRIDGE_ADDR, buf, len, false,
                                  BUS_TIMEOUT_US);
    if (got != (int)len) {
        count_err(got);
        if (got == PICO_ERROR_TIMEOUT) {
            bus_recover();
        }
        return -1;
    }
    return got;
}

int bus_read_reg_nostop(uint8_t reg, uint8_t *buf, size_t len)
{
    if (!tx_reg(reg, NULL, 0u, true)) {
        bus_recover();
        return -1;
    }

    int got = i2c_read_timeout_us(I2C_INST, BRIDGE_ADDR, buf, len, false,
                                  BUS_TIMEOUT_US);
    if (got != (int)len) {
        bus_recover();
        return -1;
    }
    return got;
}

int bus_read_only(uint8_t *buf, size_t len)
{
    int got = i2c_read_timeout_us(I2C_INST, BRIDGE_ADDR, buf, len, false,
                                  BUS_TIMEOUT_US);

    if (got != (int)len) {
        count_err(got);
        if (got == PICO_ERROR_TIMEOUT) {
            bus_recover();
        }
    }
    return (got == (int)len) ? got : -1;
}

uint16_t map_cycle(const uint8_t *map)
{
    return (uint16_t)((uint16_t)map[REG_CYCLE_L] |
                      ((uint16_t)map[REG_CYCLE_H] << 8));
}

bool map_read(uint8_t *map)
{
    return bus_read_reg(REG_MAGIC, map, 256u) == 256;
}

bool map_stable(uint8_t *map, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    do {
        uint8_t seq_before;
        uint8_t seq_after;

        if ((bus_read_reg(REG_SEQ, &seq_before, 1u) != 1) ||
            !map_read(map) ||
            (bus_read_reg(REG_SEQ, &seq_after, 1u) != 1)) {
            sleep_ms(1u);
            continue;
        }
        if ((seq_before == seq_after) && ((seq_before & 1u) == 0u)) {
            return true;
        }
    } while (absolute_time_diff_us(get_absolute_time(), deadline) > 0);

    return false;
}

bool map_wait_cycle(uint16_t old_cycle, uint8_t *map, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    do {
        uint8_t raw[2];

        if (bus_read_reg(REG_CYCLE_L, raw, 2u) == 2) {
            uint16_t cycle = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));

            if (cycle != old_cycle) {
                return map_stable(map, 2000u);
            }
        }
        sleep_us(2000u);
    } while (absolute_time_diff_us(get_absolute_time(), deadline) > 0);

    return false;
}

bool map_wait_not_busy(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    do {
        uint8_t flags;

        if ((bus_read_reg(REG_FLAGS, &flags, 1u) == 1) &&
            ((flags & FLAG_BUSY) == 0u)) {
            return true;
        }
        sleep_us(1000u);
    } while (absolute_time_diff_us(get_absolute_time(), deadline) > 0);

    return false;
}

bool map_wait_busy(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    do {
        uint8_t flags;

        if ((bus_read_reg(REG_FLAGS, &flags, 1u) == 1) &&
            ((flags & FLAG_BUSY) != 0u)) {
            return true;
        }
        sleep_us(500u);
    } while (absolute_time_diff_us(get_absolute_time(), deadline) > 0);

    return false;
}

bool map_wait_snapshot(uint8_t *map, uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    do {
        if (map_stable(map, 1000u) &&
            (map[REG_MAGIC] == 0x5Au) &&
            ((map[REG_FLAGS] & FLAG_SNAPSHOT) != 0u)) {
            return true;
        }
        sleep_us(5000u);
    } while (absolute_time_diff_us(get_absolute_time(), deadline) > 0);

    return false;
}

bool write_speed(uint8_t value)
{
    return bus_write_reg(REG_SPEED, &value, 1u) == 1;
}

bool send_cmd(uint8_t cmd)
{
    return bus_write_reg(REG_CMD, &cmd, 1u) == 1;
}

bool select_sensor(uint8_t index)
{
    return bus_write_reg(REG_SELECTED, &index, 1u) == 1;
}

bool read_scratch(uint8_t *dst9)
{
    return bus_read_reg(REG_SCRATCH, dst9, 9u) == 9;
}
