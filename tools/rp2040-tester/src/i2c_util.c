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

volatile uint32_t bus_txn;
volatile uint32_t bus_health_frames;
volatile uint32_t bus_health_missed;
volatile uint32_t bus_health_truncated;
volatile uint8_t  bus_health_flags;
volatile uint8_t  bus_health_err_bus;

static uint32_t frames_last;
static bool     frames_primed;
static uint32_t fail_base;

void bus_stats_reset(void)
{
    bus_err_timeout = 0u;
    bus_err_nack = 0u;
    bus_err_other = 0u;
}

static uint32_t failures_total(void)
{
    return bus_err_nack + bus_err_timeout + bus_err_other;
}

/* One transaction reads the whole health block: 32-bit frame counter, sticky
 * flags and the saturating BUSERR count.  The counter only changes at an
 * address match, so the four bytes cannot tear. */
static bool health_read(uint8_t *b, size_t len)
{
    return bus_read_reg(REG_FRAMES, b, len) == (int)len;
}

static uint32_t health_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool bus_health_sample(void)
{
    uint8_t b[6];

    if (!health_read(b, sizeof(b))) {
        return false;
    }

    uint32_t now = health_u32(b);

    if (!frames_primed) {
        frames_last = now;
        frames_primed = true;
    } else {
        uint32_t delta = now - frames_last;

        /* A slave reset restarts the counter at zero.  Treat an implausible
         * jump as a new baseline instead of inventing 4 billion lost frames. */
        if (delta > 1000000u) {
            bus_txn = 0u;
            bus_health_frames = 0u;
            bus_health_missed = 0u;
            bus_health_truncated = 0u;
            fail_base = failures_total();
        } else {
            bus_health_frames += delta;
        }
        frames_last = now;
    }
    bus_health_flags = b[4];
    bus_health_err_bus = b[5];

    uint32_t failures = failures_total() - fail_base;

    bus_health_missed = (bus_txn > bus_health_frames)
                        ? bus_txn - bus_health_frames : 0u;
    bus_health_truncated = (failures > bus_health_missed)
                           ? failures - bus_health_missed : 0u;
    return true;
}

void bus_health_reset(void)
{
    uint8_t b[6];

    frames_primed = false;
    for (int attempt = 0; (attempt < 3) && !frames_primed; attempt++) {
        if (health_read(b, sizeof(b))) {
            frames_last = health_u32(b);
            frames_primed = true;
        }
    }

    bus_txn = 0u;
    bus_health_frames = 0u;
    bus_health_missed = 0u;
    bus_health_truncated = 0u;
    bus_health_flags = 0u;
    bus_health_err_bus = 0u;
    fail_base = failures_total();
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

void bus_hold_sda_low(uint32_t ms)
{
    gpio_set_function(PIN_SDA, GPIO_FUNC_SIO);
    gpio_set_dir(PIN_SDA, GPIO_OUT);
    gpio_put(PIN_SDA, 0);
    sleep_ms(ms);
    gpio_put(PIN_SDA, 1);
    gpio_set_dir(PIN_SDA, GPIO_IN);
    busy_wait_us_32(100);

    i2c_init(I2C_INST, current_speed);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    bus_recover();
}

bool bus_probe(uint8_t addr)
{
    uint8_t b = 0u;
    int ret = i2c_read_timeout_us(I2C_INST, addr, &b, 1u, false, BUS_PROBE_US);

    if (ret == PICO_ERROR_TIMEOUT) {
        bus_recover();
    }
    if (ret == 1) {
        bus_txn++;
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

    bus_txn++;
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

    bus_txn++;
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

    bus_txn++;
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
    bus_txn++;
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
    bool ok = bus_write_reg(REG_CMD, &cmd, 1u) == 1;

    /* Clearing the slave counters invalidates the master-side baseline: the
     * frame counter restarts at zero, so a stale baseline would look like a
     * 64K wrap.  Re-prime immediately. */
    if (ok && (cmd == CMD_CLEAR_COUNTERS)) {
        bus_stats_reset();
        bus_health_reset();
    }
    return ok;
}

bool select_sensor(uint8_t index)
{
    return bus_write_reg(REG_SELECTED, &index, 1u) == 1;
}

bool bus_fault(uint8_t code)
{
    return bus_write_reg(REG_FAULT, &code, 1u) == 1;
}

/* ---- bit-banged fault injection ---------------------------------------- */

static void bb_delay(void)
{
    busy_wait_us_32(5);
}

static void bb_sda(int high)
{
    if (high) {
        gpio_set_dir(PIN_SDA, GPIO_IN);
    } else {
        gpio_put(PIN_SDA, 0);
        gpio_set_dir(PIN_SDA, GPIO_OUT);
    }
}

static void bb_scl(int high)
{
    if (high) {
        gpio_set_dir(PIN_SCL, GPIO_IN);
    } else {
        gpio_put(PIN_SCL, 0);
        gpio_set_dir(PIN_SCL, GPIO_OUT);
    }
}

static void bb_begin(void)
{
    gpio_set_function(PIN_SDA, GPIO_FUNC_SIO);
    gpio_set_function(PIN_SCL, GPIO_FUNC_SIO);
    gpio_pull_up(PIN_SDA);
    gpio_pull_up(PIN_SCL);
    bb_sda(1);
    bb_scl(1);
    bb_delay();
}

static void bb_end(void)
{
    bb_sda(1);
    bb_scl(1);
    bb_delay();
    i2c_init(I2C_INST, current_speed);
    gpio_set_function(PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_SCL, GPIO_FUNC_I2C);
    bus_recover();
}

static void bb_start(void)
{
    bb_sda(1);
    bb_scl(1);
    bb_delay();
    bb_sda(0);
    bb_delay();
    bb_scl(0);
    bb_delay();
}

static void bb_stop(void)
{
    bb_sda(0);
    bb_delay();
    bb_scl(1);
    bb_delay();
    bb_sda(1);
    bb_delay();
}

/* Returns true when the slave acknowledged the byte. */
static bool bb_byte(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        bb_sda((b >> i) & 1u);
        bb_delay();
        bb_scl(1);
        bb_delay();
        bb_scl(0);
        bb_delay();
    }
    bb_sda(1);
    bb_delay();
    bb_scl(1);
    bb_delay();
    bool ack = !gpio_get(PIN_SDA);
    bb_scl(0);
    bb_delay();
    return ack;
}

bool bus_bb_corrupt_addr(uint8_t addr7, int flip_bit)
{
    uint8_t addr = (uint8_t)(addr7 << 1);

    if ((flip_bit > 0) && (flip_bit < 7)) {
        addr ^= (uint8_t)(1u << flip_bit);
    }

    bb_begin();
    bus_txn++;                      /* an address phase was attempted */
    bb_start();
    bool ack = bb_byte(addr);
    bb_byte(0x00);
    bb_stop();
    bb_end();
    return ack;
}

bool bus_bb_truncate(uint8_t addr7, uint8_t reg, int data_bits)
{
    bb_begin();
    bus_txn++;
    bb_start();
    bool ack = bb_byte((uint8_t)(addr7 << 1));
    if (ack) {
        ack = bb_byte(reg);
    }
    for (int i = 0; (ack) && (i < data_bits) && (i < 8); i++) {
        bb_sda((0x5Au >> (7 - i)) & 1u);
        bb_delay();
        bb_scl(1);
        bb_delay();
        bb_scl(0);
        bb_delay();
    }
    bb_stop();
    bb_end();
    return ack;
}

/* Write a register with one bit of the data byte flipped, so the slave sees a
 * valid frame carrying a corrupted value. */
bool bus_bb_corrupt_write(uint8_t addr7, uint8_t reg, uint8_t value,
                          int flip_bit)
{
    uint8_t v = value;

    if ((flip_bit >= 0) && (flip_bit < 8)) {
        v ^= (uint8_t)(1u << flip_bit);
    }

    bb_begin();
    bus_txn++;
    bb_start();
    bool ack = bb_byte((uint8_t)(addr7 << 1));
    if (ack) {
        ack = bb_byte(reg);
    }
    if (ack) {
        (void)bb_byte(v);
    }
    bb_stop();
    bb_end();
    return ack;
}

void bus_bb_extra_clock(void)
{
    bb_begin();
    bb_scl(0);
    bb_delay();
    bb_scl(1);
    bb_delay();
    bb_end();
}

bool read_scratch(uint8_t *dst9)
{
    return bus_read_reg(REG_SCRATCH, dst9, 9u) == 9;
}
