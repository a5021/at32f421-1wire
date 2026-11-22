#include "onewire.h"
#include "ow_hal.h"
#include "ow_crc.h"

#include <string.h>

#define OW_CMD_SEARCH_ROM       0xF0u

static uint8_t ow_tx[OW_MAX_SLOTS + 1u];
static volatile uint8_t ow_rx[OW_MAX_SLOTS];
static volatile uint8_t ow_pair[2];
static uint16_t ow_rx_slots;
static uint16_t ow_rx_pos;      /* unpacked bytes, resumed across polls */

/* Search ROM phases. */
enum {
    OS_RESET_ARM = 0,
    OS_RESET_WAIT,
    OS_CMD_WAIT,
    OS_PAIR_ARM,
    OS_PAIR_WAIT,
    OS_CHECK,
    OS_DIR_WAIT,
    OS_COMPLETE,
    OS_FINISHED
};

void ow_reset_start(void)
{
    ow_hal_reset();
}

ow_status_t ow_reset_poll(bool *presence)
{
    if (!ow_hal_done()) {
        return OW_BUSY;
    }
    if (presence != 0) {
        *presence = (ow_hal_captured(2u) >= 2u);
    }
    return OW_OK;
}

bool ow_write_start(const uint8_t *data, uint16_t len)
{
    uint16_t slots;
    uint16_t k = 0u;

    if ((data == 0) || (len == 0u) || (len > OW_MAX_BYTES) || ow_hal_busy()) {
        return false;
    }

    slots = (uint16_t)(len * 8u);
    for (uint16_t i = 0u; i < len; i++) {
        uint8_t b = data[i];
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            ow_tx[k++] = ((b & 1u) != 0u) ? OW_T_LOW1 : OW_T_LOW0;
            b >>= 1;
        }
    }
    ow_tx[k] = 0u;

    ow_hal_feed(ow_tx, slots);
    return true;
}

ow_status_t ow_write_poll(void)
{
    return ow_hal_done() ? OW_OK : OW_BUSY;
}

/* Slot expansion in two-byte steps.  Sixteen slot stores keep the longest
 * synchronous stretch of a ten-byte command well inside the slave's budget;
 * the packet itself has no deadline until it is fired. */
#define OW_TX_CHUNK 2u
static uint16_t tx_pos;

void ow_tx_reset(void)
{
    tx_pos = 0u;
}

uint16_t ow_tx_build(const uint8_t *data, uint16_t len)
{
    uint16_t n = 0u;

    if ((data == 0) || (len == 0u) || (len > OW_MAX_BYTES)) {
        return 0u;
    }
    while ((n < OW_TX_CHUNK) && (tx_pos < len)) {
        uint8_t b = data[tx_pos];
        uint16_t k = (uint16_t)(tx_pos * 8u);

        for (uint8_t bit = 0u; bit < 8u; bit++) {
            ow_tx[k++] = ((b & 1u) != 0u) ? OW_T_LOW1 : OW_T_LOW0;
            b >>= 1;
        }
        tx_pos++;
        n++;
    }
    return (uint16_t)(len - tx_pos);
}

bool ow_tx_fire(uint16_t len)
{
    if ((len == 0u) || (len > OW_MAX_BYTES) || (tx_pos < len) ||
        ow_hal_busy()) {
        return false;
    }
    /* The DMA walks slots 1..N, so slot N is read even though only N-1
     * pulses were built.  It must be the terminator, not stale data. */
    ow_tx[len * 8u] = 0u;
    ow_hal_feed(ow_tx, (uint16_t)(len * 8u));
    return true;
}

bool ow_read_start(uint16_t len)
{
    if ((len == 0u) || (len > OW_MAX_BYTES) || ow_hal_busy()) {
        return false;
    }

    ow_rx_slots = (uint16_t)(len * 8u);
    ow_rx_pos = 0u;
    memset((void *)ow_rx, 0xFF, ow_rx_slots);
    ow_hal_read(ow_rx, ow_rx_slots);
    return true;
}

/* Three bytes per call: unpacking all nine held the CPU longer than the I2C
 * slave's service budget allows. */
#define OW_RX_CHUNK 1u

ow_status_t ow_read_poll(uint8_t *dst)
{
    uint16_t total = (uint16_t)(ow_rx_slots / 8u);
    uint16_t end = (uint16_t)(ow_rx_pos + OW_RX_CHUNK);

    if (!ow_hal_done()) {
        return OW_BUSY;
    }
    if ((dst == 0) || (ow_hal_captured(ow_rx_slots) < ow_rx_slots)) {
        return OW_BUS_ERROR;
    }

    if (end > total) {
        end = total;
    }
    for (uint16_t i = ow_rx_pos; i < end; i++) {
        uint8_t b = 0u;
        for (uint8_t bit = 0u; bit < 8u; bit++) {
            if (ow_rx[(i * 8u) + bit] <= OW_T_SAMPLE_MAX) {
                b |= (uint8_t)(1u << bit);
            }
        }
        dst[i] = b;
    }
    ow_rx_pos = end;
    return (end < total) ? OW_BUSY : OW_OK;
}

void ow_search_begin(ow_search_t *s)
{
    if (s == 0) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->phase = OS_RESET_ARM;
}

ow_status_t ow_search_poll(ow_search_t *s)
{
    if (s == 0) {
        return OW_PARAM;
    }

    switch (s->phase) {
    case OS_RESET_ARM:
        ow_reset_start();
        s->phase = OS_RESET_WAIT;
        return OW_BUSY;

    case OS_RESET_WAIT: {
        bool presence = false;
        ow_status_t st = ow_reset_poll(&presence);
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if ((st != OW_OK) || !presence) {
            s->phase = OS_FINISHED;
            return OW_END;
        }
        {
            uint8_t cmd = OW_CMD_SEARCH_ROM;
            if (!ow_write_start(&cmd, 1u)) {
                s->phase = OS_FINISHED;
                return OW_BUS_ERROR;
            }
        }
        s->phase = OS_CMD_WAIT;
        return OW_BUSY;
    }

    case OS_CMD_WAIT: {
        ow_status_t st = ow_write_poll();
        if (st == OW_BUSY) {
            return OW_BUSY;
        }
        if (st != OW_OK) {
            s->phase = OS_FINISHED;
            return st;
        }
        s->bit = 1u;
        s->last_zero = 0u;
        s->rom_byte = 0u;
        s->rom_mask = 1u;
        s->phase = OS_PAIR_ARM;
        return OW_BUSY;
    }

    case OS_PAIR_ARM:
        ow_hal_read_pair(ow_pair);
        s->phase = OS_PAIR_WAIT;
        return OW_BUSY;

    case OS_PAIR_WAIT: {
        uint16_t got;
        if (!ow_hal_done()) {
            return OW_BUSY;
        }
        got = ow_hal_captured(2u);
        s->id_bit = ((got >= 1u) && (ow_pair[0] <= OW_T_SAMPLE_MAX)) ? 1u : 0u;
        s->cmp_bit = ((got >= 2u) && (ow_pair[1] <= OW_T_SAMPLE_MAX)) ? 1u : 0u;
        s->phase = OS_CHECK;
        return OW_BUSY;
    }

    case OS_CHECK:
        if ((s->id_bit != 0u) && (s->cmp_bit != 0u)) {
            s->phase = OS_FINISHED;
            return OW_END;
        }
        if (s->id_bit != s->cmp_bit) {
            s->dir = s->id_bit;
        } else {
            if (s->bit < (uint8_t)s->last_discrepancy) {
                s->dir = ((s->rom[s->rom_byte] & s->rom_mask) != 0u) ? 1u : 0u;
            } else {
                s->dir = (s->bit == (uint8_t)s->last_discrepancy) ? 1u : 0u;
            }
            if (s->dir == 0u) {
                s->last_zero = s->bit;
            }
        }

        if (s->dir != 0u) {
            s->rom[s->rom_byte] |= s->rom_mask;
        } else {
            s->rom[s->rom_byte] &= (uint8_t)~s->rom_mask;
        }

        {
            static uint8_t dir_pulse[2];
            dir_pulse[0] = (s->dir != 0u) ? OW_T_LOW1 : OW_T_LOW0;
            dir_pulse[1] = 0u;
            ow_hal_feed(dir_pulse, 1u);
        }
        s->phase = OS_DIR_WAIT;
        return OW_BUSY;

    case OS_DIR_WAIT:
        if (!ow_hal_done()) {
            return OW_BUSY;
        }
        s->bit++;
        s->rom_mask <<= 1;
        if (s->rom_mask == 0u) {
            s->rom_byte++;
            s->rom_mask = 1u;
        }
        if (s->rom_byte >= 8u) {
            s->phase = OS_COMPLETE;
        } else {
            s->phase = OS_PAIR_ARM;
        }
        return OW_BUSY;

    case OS_COMPLETE:
        if (ow_crc8(s->rom, 7u) != s->rom[7]) {
            s->phase = OS_FINISHED;
            return OW_CRC_ERROR;
        }
        s->last_discrepancy = (int8_t)s->last_zero;
        s->last_device = (s->last_zero == 0u);
        s->phase = s->last_device ? OS_FINISHED : OS_RESET_ARM;
        return OW_OK;

    case OS_FINISHED:
    default:
        return OW_END;
    }
}

void ow_delay_start(uint32_t us)
{
    ow_hal_wait(us);
}

bool ow_delay_poll(void)
{
    return ow_hal_done();
}
