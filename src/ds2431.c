#include "ds2431.h"
#include "ow_hal.h"
#include "ow_crc.h"

#define DS2431_CMD_MATCH_ROM        0x55u
#define DS2431_CMD_SKIP_ROM         0xCCu
#define DS2431_CMD_READ_MEMORY      0xF0u
#define DS2431_CMD_WRITE_SCRATCH    0x0Fu
#define DS2431_CMD_READ_SCRATCH     0xAAu
#define DS2431_CMD_COPY_SCRATCH     0x55u
#define DS2431_COPY_OK              0xAAu

#define DS2431_ES_MASK              0x07u
#define DS2431_PF_MASK              0x20u

enum {
    PH_RESET_ARM = 0,
    PH_RESET_WAIT,
    PH_CMD_BUILD,
    PH_CMD_WAIT,
    PH_READ_WAIT,
    PH_SP_RESET_WAIT,
    PH_SP_CMD_WAIT,
    PH_SP_READ_WAIT,
    PH_COPY_RESET_WAIT,
    PH_COPY_CMD_WAIT,
    PH_COPY_DELAY,
    PH_STATUS_READ,
    PH_DONE
};

static void put_addr(uint8_t *p, uint16_t addr)
{
    p[0] = (uint8_t)(addr & 0xFFu);
    p[1] = (uint8_t)(addr >> 8);
}

static bool scratchpad_ok(const ds2431_t *d)
{
    uint8_t crc_in[12];
    uint16_t expected;
    uint16_t got;

    if ((d->sp[0] != (uint8_t)(d->addr & 0xFFu)) ||
        (d->sp[1] != (uint8_t)(d->addr >> 8))) {
        return false;
    }
    if ((d->sp[2] & DS2431_ES_MASK) != d->es) {
        return false;
    }
    if ((d->sp[2] & DS2431_PF_MASK) != 0u) {
        return false;
    }

    crc_in[0] = DS2431_CMD_READ_SCRATCH;
    for (uint8_t i = 0u; i < 11u; i++) {
        crc_in[i + 1u] = d->sp[i];
    }
    expected = (uint16_t)(~ow_crc16(crc_in, 12u));
    got = (uint16_t)((uint16_t)d->sp[11] | ((uint16_t)d->sp[12] << 8));
    return got == expected;
}

static uint16_t rom_prefix(const ds2431_t *d, uint8_t *p)
{
    if (d->match_rom) {
        p[0] = DS2431_CMD_MATCH_ROM;
        for (uint8_t i = 0u; i < 8u; i++) {
            p[1u + i] = d->rom[i];
        }
        return 9u;
    }
    p[0] = DS2431_CMD_SKIP_ROM;
    return 1u;
}

static uint16_t read_cmd(const ds2431_t *d, uint8_t *cmd)
{
    uint16_t n = rom_prefix(d, cmd);

    cmd[n++] = DS2431_CMD_READ_MEMORY;
    put_addr(&cmd[n], d->addr);
    return (uint16_t)(n + 2u);
}

static uint16_t write_cmd(const ds2431_t *d, uint8_t *cmd)
{
    uint16_t n = rom_prefix(d, cmd);

    cmd[n++] = DS2431_CMD_WRITE_SCRATCH;
    put_addr(&cmd[n], d->addr);
    n += 2u;
    for (uint8_t i = 0u; i < DS2431_SCRATCH_SIZE; i++) {
        cmd[n + i] = d->src[i];
    }
    return (uint16_t)(n + DS2431_SCRATCH_SIZE);
}

void ds2431_init(ds2431_t *d)
{
    if (d == 0) {
        return;
    }
    d->status = 0u;
    d->phase = PH_DONE;
    d->mode = DS2431_PH_IDLE;
    d->len = 0u;
    d->es = 0u;
    d->addr = 0u;
    d->dst = 0;
    d->src = 0;
    d->match_rom = false;
    for (uint8_t i = 0u; i < 8u; i++) {
        d->rom[i] = 0u;
    }
}

static bool idle(const ds2431_t *d)
{
    return (d != 0) && (d->phase == PH_DONE) && !ow_hal_busy();
}

bool ds2431_read_start(ds2431_t *d, const uint8_t *rom, uint16_t addr,
                       uint8_t *dst, uint8_t len)
{
    if (dst == 0) {
        return false;
    }
    if ((len == 0u) || (len > OW_MAX_BYTES) ||
        (uint32_t)addr + len > DS2431_EEPROM_SIZE) {
        return false;
    }
    if (!idle(d)) {
        return false;
    }

    d->mode = DS2431_PH_READ;
    d->match_rom = (rom != 0);
    if (rom != 0) {
        for (uint8_t i = 0u; i < 8u; i++) {
            d->rom[i] = rom[i];
        }
    }
    d->addr = addr;
    d->dst = dst;
    d->len = len;
    d->phase = PH_RESET_ARM;
    return true;
}

ow_status_t ds2431_read_poll(ds2431_t *d)
{
    if (d == 0) {
        return OW_PARAM;
    }
    if (d->mode != DS2431_PH_READ) {
        return (d->mode == DS2431_PH_IDLE) ? OW_OK : OW_PARAM;
    }

    switch (d->phase) {
    case PH_RESET_ARM:
        ow_reset_start();
        d->cmd_len = (uint8_t)read_cmd(d, d->sp);
        d->phase = PH_RESET_WAIT;
        return OW_BUSY;

    case PH_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = PH_DONE;
            return OW_NO_PRESENCE;
        }
        ow_tx_reset();
        d->phase = PH_CMD_BUILD;
        return OW_BUSY;
    }

    case PH_CMD_BUILD:
        if (ow_tx_build(d->sp, d->cmd_len) != 0u) {
            return OW_BUSY;
        }
        if (!ow_tx_fire(d->cmd_len)) {
            d->phase = PH_DONE;
            return OW_PARAM;
        }
        d->phase = PH_CMD_WAIT;
        return OW_BUSY;

    case PH_CMD_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = PH_DONE;
            return st;
        }
        if (!ow_read_start(d->len)) {
            d->phase = PH_DONE;
            return OW_PARAM;
        }
        d->phase = PH_READ_WAIT;
        return OW_BUSY;
    }

    case PH_READ_WAIT: {
        ow_status_t st = ow_read_poll(d->dst);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        d->phase = PH_DONE;
        return st;
    }

    case PH_DONE:
    default:
        return OW_OK;
    }
}

bool ds2431_write_start(ds2431_t *d, const uint8_t *rom, uint16_t addr,
                        const uint8_t *src)
{
    if (src == 0) {
        return false;
    }
    if ((addr >= DS2431_EEPROM_SIZE) ||
        ((addr & (DS2431_BLOCK_SIZE - 1u)) != 0u) ||
        ((uint32_t)addr + DS2431_SCRATCH_SIZE > DS2431_EEPROM_SIZE)) {
        return false;
    }
    if (!idle(d)) {
        return false;
    }

    d->mode = DS2431_PH_WRITE;
    d->match_rom = (rom != 0);
    if (rom != 0) {
        for (uint8_t i = 0u; i < 8u; i++) {
            d->rom[i] = rom[i];
        }
    }
    d->addr = addr;
    d->src = src;
    d->len = DS2431_SCRATCH_SIZE;
    d->es = DS2431_SCRATCH_SIZE - 1u;
    d->status = 0u;
    d->phase = PH_RESET_ARM;
    return true;
}

ow_status_t ds2431_write_poll(ds2431_t *d)
{
    if (d == 0) {
        return OW_PARAM;
    }
    if (d->mode != DS2431_PH_WRITE) {
        return (d->mode == DS2431_PH_IDLE) ? OW_OK : OW_PARAM;
    }

    switch (d->phase) {
    case PH_RESET_ARM:
        ow_hal_strong_pullup(false);
        ow_reset_start();
        d->phase = PH_RESET_WAIT;
        return OW_BUSY;

    case PH_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = PH_DONE;
            return OW_NO_PRESENCE;
        }
        /* Write slots only: the pad may drive high between slots. */
        ow_hal_strong_pullup(true);
        {
            uint8_t cmd[20];
            uint16_t n = write_cmd(d, cmd);
            if (!ow_write_start(cmd, n)) {
                ow_hal_strong_pullup(false);
                d->phase = PH_DONE;
                return OW_PARAM;
            }
        }
        d->phase = PH_CMD_WAIT;
        return OW_BUSY;
    }

    case PH_CMD_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        ow_hal_strong_pullup(false);
        if (st != OW_OK) {
            d->phase = PH_DONE;
            return st;
        }
        ow_reset_start();
        d->phase = PH_SP_RESET_WAIT;
        return OW_BUSY;
    }

    case PH_SP_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = PH_DONE;
            return OW_NO_PRESENCE;
        }
        {
            uint8_t cmd[10];
            uint16_t n = rom_prefix(d, cmd);
            cmd[n++] = DS2431_CMD_READ_SCRATCH;
            if (!ow_write_start(cmd, n)) {
                d->phase = PH_DONE;
                return OW_PARAM;
            }
        }
        d->phase = PH_SP_CMD_WAIT;
        return OW_BUSY;
    }

    case PH_SP_CMD_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = PH_DONE;
            return st;
        }
        if (!ow_read_start(13u)) {
            d->phase = PH_DONE;
            return OW_PARAM;
        }
        d->phase = PH_SP_READ_WAIT;
        return OW_BUSY;
    }

    case PH_SP_READ_WAIT: {
        ow_status_t st = ow_read_poll(d->sp);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            d->phase = PH_DONE;
            return st;
        }
        if (!scratchpad_ok(d)) {
            d->phase = PH_DONE;
            return OW_CRC_ERROR;
        }
        ow_reset_start();
        d->phase = PH_COPY_RESET_WAIT;
        return OW_BUSY;
    }

    case PH_COPY_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            d->phase = PH_DONE;
            return OW_NO_PRESENCE;
        }
        ow_hal_strong_pullup(true);
        {
            uint8_t cmd[13];
            uint16_t n = rom_prefix(d, cmd);
            cmd[n++] = DS2431_CMD_COPY_SCRATCH;
            put_addr(&cmd[n], d->addr);
            n += 2u;
            cmd[n++] = d->es;
            if (!ow_write_start(cmd, n)) {
                ow_hal_strong_pullup(false);
                d->phase = PH_DONE;
                return OW_PARAM;
            }
        }
        d->phase = PH_COPY_CMD_WAIT;
        return OW_BUSY;
    }

    case PH_COPY_CMD_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            ow_hal_strong_pullup(false);
            d->phase = PH_DONE;
            return st;
        }
        ow_delay_start(DS2431_TPROG_US);
        d->phase = PH_COPY_DELAY;
        return OW_BUSY;
    }

    case PH_COPY_DELAY:
        if (!ow_delay_poll()) {
            return OW_BUSY;
        }
        /* Read slots need the open-drain pad: the device answers low. */
        ow_hal_strong_pullup(false);
        if (!ow_read_start(1u)) {
            d->phase = PH_DONE;
            return OW_PARAM;
        }
        d->phase = PH_STATUS_READ;
        return OW_BUSY;

    case PH_STATUS_READ: {
        ow_status_t st = ow_read_poll(&d->status);
        d->phase = PH_DONE;
        if (st != OW_OK) {
            return st;
        }
        return (d->status == DS2431_COPY_OK) ? OW_OK : OW_BUS_ERROR;
    }

    case PH_DONE:
    default:
        return OW_OK;
    }
}
