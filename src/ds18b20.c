#include "ds18b20.h"
#include "ow_hal.h"
#include "ow_crc.h"

#include <string.h>

#define DS18B20_CMD_MATCH_ROM   0x55u
#define DS18B20_CMD_SKIP_ROM    0xCCu
#define DS18B20_CMD_CONVERT_T   0x44u
#define DS18B20_CMD_READ_SP     0xBEu
#define DS18B20_CMD_READ_POWER  0xB4u

enum {
    DS_PH_RESET_ARM = 0,
    DS_PH_RESET_WAIT,
    DS_PH_WRITE_BUILD,
    DS_PH_WRITE_WAIT,
    DS_PH_DELAY_WAIT,
    DS_PH_READ_WAIT,
    DS_PH_DONE
};

static uint16_t build_prefix(const ds18b20_t *d, uint8_t *cmd)
{
    uint16_t n = 0u;

    if (d->match_rom) {
        cmd[n++] = DS18B20_CMD_MATCH_ROM;
        memcpy(&cmd[n], d->rom, 8u);
        n += 8u;
    } else {
        cmd[n++] = DS18B20_CMD_SKIP_ROM;
    }
    return n;
}

void ds18b20_init(ds18b20_t *d, bool parasite)
{
    if (d == 0) {
        return;
    }
    memset(d, 0, sizeof(*d));
    d->parasite = parasite;
    d->conv_us = DS18B20_CONV_US_12;
    d->phase = DS_PH_DONE;
}

bool ds18b20_convert_start(ds18b20_t *d, const uint8_t *rom)
{
    if ((d == 0) || (d->phase != DS_PH_DONE) || ow_hal_busy()) {
        return false;
    }
    d->match_rom = (rom != 0);
    if (rom != 0) {
        memcpy(d->rom, rom, 8u);
    }
    d->stage = 0u;
    d->phase = DS_PH_RESET_ARM;
    return true;
}

static ow_status_t convert_poll_raw(ds18b20_t *d)
{
    if (d == 0) {
        return OW_PARAM;
    }

    switch (d->phase) {
    case DS_PH_RESET_ARM:
        if (d->parasite) {
            ow_hal_strong_pullup(false);
        }
        ow_reset_start();
        d->phase = DS_PH_RESET_WAIT;
        return OW_BUSY;

    case DS_PH_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = DS_PH_DONE;
            return OW_NO_PRESENCE;
        }

        /* Parasite power: the slave does not drive during a write slot, so the
         * pad can be push-pull for the whole command phase.  This also gets
         * power onto the bus before the first bit of Convert T. */
        if (d->parasite) {
            ow_hal_strong_pullup(true);
        }

        {
            uint16_t n = build_prefix(d, d->cmd);
            d->cmd[n++] = DS18B20_CMD_CONVERT_T;
            d->cmd_len = (uint8_t)n;
        }
        ow_tx_reset();
        d->phase = DS_PH_WRITE_BUILD;
        return OW_BUSY;
    }

    case DS_PH_WRITE_BUILD:
        if (ow_tx_build(d->cmd, d->cmd_len) != 0u) {
            return OW_BUSY;
        }
        if (!ow_tx_fire(d->cmd_len)) {
            d->phase = DS_PH_DONE;
            if (d->parasite) {
                ow_hal_strong_pullup(false);
            }
            return OW_PARAM;
        }
        d->phase = DS_PH_WRITE_WAIT;
        return OW_BUSY;

    case DS_PH_WRITE_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = DS_PH_DONE;
            if (d->parasite) {
                ow_hal_strong_pullup(false);
            }
            return st;
        }
        ow_delay_start(d->conv_us);
        d->phase = DS_PH_DELAY_WAIT;
        return OW_BUSY;
    }

    case DS_PH_DELAY_WAIT:
        if (!ow_delay_poll()) {
            return OW_BUSY;
        }
        if (d->parasite) {
            ow_hal_strong_pullup(false);
        }
        d->phase = DS_PH_DONE;
        return OW_OK;

    case DS_PH_DONE:
    default:
        /* A second poll must report the result the operation ended with, not
         * a fresh success: OW_NO_PRESENCE would turn into OW_OK here. */
        return (ow_status_t)d->result;
    }
}

ow_status_t ds18b20_convert_poll(ds18b20_t *d)
{
    ow_status_t st = convert_poll_raw(d);

    if ((d != 0) && (st != OW_BUSY)) {
        d->result = (uint8_t)st;
    }
    return st;
}

bool ds18b20_power_supply_start(ds18b20_t *d, const uint8_t *rom)
{
    if ((d == 0) || (d->phase != DS_PH_DONE) || ow_hal_busy()) {
        return false;
    }
    d->match_rom = (rom != 0);
    if (rom != 0) {
        memcpy(d->rom, rom, 8u);
    }
    d->phase = DS_PH_RESET_ARM;
    return true;
}

static ow_status_t power_supply_poll_raw(ds18b20_t *d)
{
    if (d == 0) {
        return OW_PARAM;
    }

    switch (d->phase) {
    case DS_PH_RESET_ARM:
        ow_hal_strong_pullup(false);
        ow_reset_start();
        d->phase = DS_PH_RESET_WAIT;
        return OW_BUSY;

    case DS_PH_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);

        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = DS_PH_DONE;
            return OW_NO_PRESENCE;
        }
        {
            uint16_t n = build_prefix(d, d->cmd);

            d->cmd[n++] = DS18B20_CMD_READ_POWER;
            d->cmd_len = (uint8_t)n;
        }
        ow_tx_reset();
        d->phase = DS_PH_WRITE_BUILD;
        return OW_BUSY;
    }

    case DS_PH_WRITE_BUILD:
        if (ow_tx_build(d->cmd, d->cmd_len) != 0u) {
            return OW_BUSY;
        }
        if (!ow_tx_fire(d->cmd_len)) {
            d->phase = DS_PH_DONE;
            return OW_PARAM;
        }
        d->phase = DS_PH_WRITE_WAIT;
        return OW_BUSY;

    case DS_PH_WRITE_WAIT: {
        ow_status_t st = ow_write_poll();

        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = DS_PH_DONE;
            return st;
        }
        if (!ow_read_start(1u)) {
            d->phase = DS_PH_DONE;
            return OW_PARAM;
        }
        d->phase = DS_PH_READ_WAIT;
        return OW_BUSY;
    }

    case DS_PH_READ_WAIT: {
        ow_status_t st = ow_read_poll(d->sp);

        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        d->phase = DS_PH_DONE;
        return st;
    }

    case DS_PH_DONE:
    default:
        return (ow_status_t)d->result;
    }
}

ow_status_t ds18b20_power_supply_poll(ds18b20_t *d, bool *parasite)
{
    ow_status_t st = power_supply_poll_raw(d);

    if ((d != 0) && (st != OW_BUSY)) {
        d->result = (uint8_t)st;
        if ((st == OW_OK) && (parasite != 0)) {
            *parasite = (d->sp[0] & 0x01u) == 0u;
        }
    }
    return st;
}

bool ds18b20_read_start(ds18b20_t *d, const uint8_t *rom)
{
    if ((d == 0) || (d->phase != DS_PH_DONE) || ow_hal_busy()) {
        return false;
    }
    d->match_rom = (rom != 0);
    if (rom != 0) {
        memcpy(d->rom, rom, 8u);
    }
    d->phase = DS_PH_RESET_ARM;
    return true;
}

static ow_status_t read_poll_raw(ds18b20_t *d);

ow_status_t ds18b20_read_poll(ds18b20_t *d)
{
    ow_status_t st = read_poll_raw(d);

    if ((d != 0) && (st != OW_BUSY)) {
        d->result = (uint8_t)st;
    }
    return st;
}

static ow_status_t read_poll_raw(ds18b20_t *d)
{
    if (d == 0) {
        return OW_PARAM;
    }

    switch (d->phase) {
    case DS_PH_RESET_ARM:
        if (d->parasite) {
            ow_hal_strong_pullup(false);        /* open-drain for presence */
        }
        ow_reset_start();
        d->phase = DS_PH_RESET_WAIT;
        return OW_BUSY;

    case DS_PH_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = DS_PH_DONE;
            return OW_NO_PRESENCE;
        }

        {
            uint16_t n = build_prefix(d, d->cmd);
            d->cmd[n++] = DS18B20_CMD_READ_SP;
            d->cmd_len = (uint8_t)n;
        }
        ow_tx_reset();
        d->phase = DS_PH_WRITE_BUILD;
        return OW_BUSY;
    }

    case DS_PH_WRITE_BUILD:
        if (ow_tx_build(d->cmd, d->cmd_len) != 0u) {
            return OW_BUSY;
        }
        if (!ow_tx_fire(d->cmd_len)) {
            d->phase = DS_PH_DONE;
            return OW_PARAM;
        }
        d->phase = DS_PH_WRITE_WAIT;
        return OW_BUSY;

    case DS_PH_WRITE_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = DS_PH_DONE;
            return st;
        }
        if (!ow_read_start(9u)) {
            d->phase = DS_PH_DONE;
            return OW_PARAM;
        }
        d->phase = DS_PH_READ_WAIT;
        return OW_BUSY;
    }

    case DS_PH_READ_WAIT: {
        ow_status_t st = ow_read_poll(d->sp);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        d->phase = DS_PH_DONE;
        if (st != OW_OK) {
            return st;
        }
        if (!ds18b20_crc_ok(d)) {
            return OW_CRC_ERROR;
        }
        d->raw = (int16_t)(((uint16_t)d->sp[1] << 8) | d->sp[0]);
        return OW_OK;
    }

    case DS_PH_DONE:
    default:
        return (ow_status_t)d->result;
    }
}

void ds18b20_temp_start(ds18b20_t *d, const uint8_t *rom)
{
    if ((d == 0) || (d->phase != DS_PH_DONE) || ow_hal_busy()) {
        return;
    }
    ds18b20_convert_start(d, rom);
    d->stage = 0u;
}

ow_status_t ds18b20_temp_poll(ds18b20_t *d, int16_t *raw)
{
    if (d == 0) {
        return OW_PARAM;
    }

    if (d->stage == 0u) {
        /* A convert that failed (or that was never started) leaves the phase
         * at DS_PH_DONE.  Reading here would hand out the previous
         * scratchpad as if it were fresh, so require a new temp_start. */
        if (d->phase == DS_PH_DONE) {
            return OW_PARAM;
        }
        {
            ow_status_t st = ds18b20_convert_poll(d);

            if (st == OW_BUSY) {
                return OW_BUSY;
            }
            if (st != OW_OK) {
                d->stage = 0u;
                return st;
            }
        }
        d->stage = 1u;
        d->phase = DS_PH_RESET_ARM;
        return OW_BUSY;
    }

    {
        ow_status_t st = ds18b20_read_poll(d);
        if ((st == OW_OK) && (raw != 0)) {
            *raw = d->raw;
        }
        if (st != OW_BUSY) {
            /* The operation is over either way: a second poll must not re-run
             * the read and report a fresh success. */
            d->stage = 0u;
        }
        return st;
    }
}

bool ds18b20_crc_ok(const ds18b20_t *d)
{
    if (d == 0) {
        return false;
    }
    return ow_crc8(d->sp, 8u) == d->sp[8];
}

float ds18b20_celsius(const ds18b20_t *d)
{
    if (d == 0) {
        return 0.0f;
    }
    return (float)d->raw * 0.0625f;
}
