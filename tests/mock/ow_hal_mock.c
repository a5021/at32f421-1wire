#include "ow_hal_mock.h"
#include "ow_crc.h"

#include <string.h>

enum {
    PS_ADDR = 0,
    PS_MATCH,
    PS_CMD,
    PS_PAYLOAD
};

enum {
    ARM_NONE = 0,
    ARM_RESET,
    ARM_PASS
};

static mock_dev_t g_dev[MOCK_MAX_DEV];
static int g_devs;

static int  g_presence = -1;            /* -1 auto, 0 absent, 1 present */
static int  g_reset_caps = 2;
static int  g_read_shortfall;
static int  g_busy_each;
static int  g_busy_left;
static bool g_active;
static bool g_parser = true;
static bool g_strong;
static bool g_read_strong;
static bool g_cmd_strong;
static int  g_last_wait = -1;
static int  g_last_arm;
static int  g_reset_caps_now;

static uint8_t g_pulses[MOCK_PULSE_MAX];
static int     g_feed_slots;

static uint8_t g_tx[MOCK_TX_MAX];
static int     g_txlen;

static uint8_t g_rx[MOCK_RX_MAX];
static int     g_rxlen;
static int     g_rxpos;

static uint8_t g_caps[MOCK_RX_MAX];
static int     g_caplen;
static int     g_cappos;

static int     g_ps;
static uint8_t g_frame[12];
static int     g_frame_len;
static int     g_need;
static int     g_cmd;
static uint16_t g_sel;
static bool    g_search;
static uint8_t g_prefix[8];
static int     g_search_bit;
static bool    g_mem_stream;
static uint16_t g_mem_ptr;
static int     g_mem_idx;

static mock_event_t g_log[MOCK_LOG_MAX];
static int g_loglen;

static void log_ev(uint8_t kind, uint32_t a, uint32_t b)
{
    if (g_loglen < MOCK_LOG_MAX) {
        g_log[g_loglen].kind = kind;
        g_log[g_loglen].a = a;
        g_log[g_loglen].b = b;
        g_loglen++;
    }
}

static void arm(int kind)
{
    g_active = true;
    g_busy_left = g_busy_each;
    g_last_arm = kind;
}

static void queue_rx(const uint8_t *p, int n)
{
    if (n > MOCK_RX_MAX) {
        n = MOCK_RX_MAX;
    }
    memcpy(g_rx, p, (size_t)n);
    g_rxlen = n;
    g_rxpos = 0;
}

static void sp18_refresh(mock_dev_t *d)
{
    d->sp18[8] = ow_crc8(d->sp18, 8u);
    if (d->sp_crc_bad) {
        d->sp18[8] ^= 0xFFu;
    }
}

void mock_reset(void)
{
    memset(g_dev, 0, sizeof(g_dev));
    g_devs = 0;
    g_presence = -1;
    g_reset_caps = 2;
    g_read_shortfall = 0;
    g_busy_each = 0;
    g_busy_left = 0;
    g_active = false;
    g_parser = true;
    g_strong = false;
    g_read_strong = false;
    g_cmd_strong = false;
    g_last_wait = -1;
    g_last_arm = ARM_NONE;
    g_reset_caps_now = 0;
    memset(g_pulses, 0, sizeof(g_pulses));
    g_feed_slots = 0;
    memset(g_tx, 0, sizeof(g_tx));
    g_txlen = 0;
    memset(g_rx, 0, sizeof(g_rx));
    g_rxlen = 0;
    g_rxpos = 0;
    memset(g_caps, 0, sizeof(g_caps));
    g_caplen = 0;
    g_cappos = 0;
    g_ps = PS_ADDR;
    memset(g_frame, 0, sizeof(g_frame));
    g_frame_len = 0;
    g_need = 0;
    g_cmd = 0;
    g_sel = 0;
    g_search = false;
    memset(g_prefix, 0, sizeof(g_prefix));
    g_search_bit = 0;
    g_mem_stream = false;
    g_mem_ptr = 0;
    g_mem_idx = -1;
    g_loglen = 0;
}

mock_dev_t *mock_add(mock_dev_type_t type, const uint8_t *rom)
{
    mock_dev_t *d;

    if (g_devs >= MOCK_MAX_DEV) {
        return 0;
    }
    d = &g_dev[g_devs++];
    memset(d, 0, sizeof(*d));
    d->type = type;
    if (rom != 0) {
        memcpy(d->rom, rom, 8u);
    }
    d->temp_raw = 0x0550;               /* +85.0 C, power-on value */
    d->sp18[2] = 0x4B;                  /* TH */
    d->sp18[3] = 0x46;                  /* TL */
    d->sp18[4] = 0x7F;                  /* 12-bit */
    d->sp18[5] = 0xFF;
    d->sp18[6] = 0x0C;
    d->sp18[7] = 0x10;
    d->sp18[0] = (uint8_t)(d->temp_raw & 0xFF);
    d->sp18[1] = (uint8_t)(d->temp_raw >> 8);
    sp18_refresh(d);
    d->status = 0xAA;
    d->es = 7;
    return d;
}

void mock_remove_all(void)
{
    memset(g_dev, 0, sizeof(g_dev));
    g_devs = 0;
}

mock_dev_t *mock_dev(int index)
{
    if ((index < 0) || (index >= g_devs)) {
        return 0;
    }
    return &g_dev[index];
}

int mock_dev_count(void)
{
    return g_devs;
}

void mock_set_presence(bool present)
{
    g_presence = present ? 1 : 0;
}

void mock_set_reset_captures(int captures)
{
    g_reset_caps = captures;
}

void mock_set_read_shortfall(int slots)
{
    g_read_shortfall = slots;
}

void mock_set_busy_ticks(int polls_per_pass)
{
    g_busy_each = (polls_per_pass < 0) ? 0 : polls_per_pass;
}

void mock_set_active(bool active)
{
    g_active = active;
    g_busy_left = 0;
}

void mock_parser(bool enabled)
{
    g_parser = enabled;
}

void mock_queue_read(const uint8_t *bytes, int len)
{
    queue_rx(bytes, len);
    g_mem_stream = false;
}

void mock_queue_captures(const uint8_t *values, int len)
{
    if (len > MOCK_RX_MAX) {
        len = MOCK_RX_MAX;
    }
    memcpy(g_caps, values, (size_t)len);
    g_caplen = len;
    g_cappos = 0;
}

void mock_clear_flags(void)
{
    g_read_strong = false;
    g_cmd_strong = false;
    g_last_wait = -1;
    g_feed_slots = 0;
    g_loglen = 0;
}

bool mock_busy(void)
{
    return g_active;
}

bool mock_strong(void)
{
    return g_strong;
}

bool mock_read_while_strong(void)
{
    return g_read_strong;
}

bool mock_last_cmd_strong(void)
{
    return g_cmd_strong;
}

int mock_last_wait_us(void)
{
    return g_last_wait;
}

int mock_last_feed_slots(void)
{
    return g_feed_slots;
}

const uint8_t *mock_last_pulses(void)
{
    return g_pulses;
}

const uint8_t *mock_last_tx(int *len)
{
    if (len != 0) {
        *len = g_txlen;
    }
    return g_tx;
}

int mock_event_count(void)
{
    return g_loglen;
}

const mock_event_t *mock_events(void)
{
    return g_log;
}

/* ------------------------------------------------------------------ */
/* Virtual 1-Wire bus                                                  */
/* ------------------------------------------------------------------ */

static mock_dev_t *first_selected(void)
{
    int i;

    for (i = 0; i < g_devs; i++) {
        if ((g_sel & (1u << i)) != 0u) {
            return &g_dev[i];
        }
    }
    return 0;
}

static bool prefix_match(const mock_dev_t *d, int bits)
{
    int i;

    for (i = 0; i < bits; i++) {
        int b = (d->rom[i >> 3] >> (i & 7)) & 1;
        int p = (g_prefix[i >> 3] >> (i & 7)) & 1;
        if (b != p) {
            return false;
        }
    }
    return true;
}

static void search_pair(int *id_bit, int *cmp_bit)
{
    int i;

    *id_bit = 1;
    *cmp_bit = 1;
    if (g_search_bit >= 64) {
        return;
    }
    for (i = 0; i < g_devs; i++) {
        const mock_dev_t *d = &g_dev[i];
        int b;

        if (!prefix_match(d, g_search_bit)) {
            continue;
        }
        b = (d->rom[g_search_bit >> 3] >> (g_search_bit & 7)) & 1;
        *id_bit &= b;
        *cmp_bit &= (b ^ 1);
    }
}

static void search_direction(int bit)
{
    if (g_search_bit < 64) {
        if (bit != 0) {
            g_prefix[g_search_bit >> 3] |= (uint8_t)(1u << (g_search_bit & 7));
        } else {
            g_prefix[g_search_bit >> 3] &= (uint8_t)~(1u << (g_search_bit & 7));
        }
        g_search_bit++;
    }
}

static void queue_sp31(mock_dev_t *d)
{
    uint8_t f[13];
    uint8_t c[12];
    uint16_t crc;

    f[0] = (uint8_t)(d->ta & 0xFFu);
    if (d->echo_bad) {
        f[0] ^= 0xFFu;
    }
    f[1] = (uint8_t)(d->ta >> 8);
    f[2] = (uint8_t)((d->es & 0x07u) | (d->pf ? 0x20u : 0u));
    memcpy(&f[3], d->sp31, 8u);

    c[0] = 0xAAu;
    memcpy(&c[1], f, 11u);
    crc = (uint16_t)~ow_crc16(c, 12u);
    if (d->sp_frame_crc_bad) {
        crc = (uint16_t)~crc;
    }
    f[11] = (uint8_t)(crc & 0xFFu);
    f[12] = (uint8_t)(crc >> 8);

    queue_rx(f, 13);
}

static void execute_cmd(int cmd, const uint8_t *p)
{
    mock_dev_t *d = first_selected();
    int i;

    if (d == 0) {
        return;
    }

    if (d->type == MOCK_DS18B20) {
        switch (cmd) {
        case 0x44:
            for (i = 0; i < g_devs; i++) {
                if ((g_sel & (1u << i)) != 0u) {
                    if (g_dev[i].type == MOCK_DS18B20) {
                        g_dev[i].converting = true;
                    }
                }
            }
            break;
        case 0xBE:
            queue_rx(d->sp18, 9);
            break;
        default:
            break;
        }
        return;
    }

    /* DS2431 */
    switch (cmd) {
    case 0xF0:
        d->ta = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        g_mem_stream = true;
        g_mem_ptr = d->ta;
        g_mem_idx = (int)(d - g_dev);
        break;
    case 0x0F:
        d->ta = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        d->latched_ta = d->ta;
        d->es = 7u;
        d->pf = d->force_pf;
        memcpy(d->sp31, &p[2], 8u);
        break;
    case 0xAA:
        queue_sp31(d);
        break;
    case 0x55: {
        uint16_t ta = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        bool ok = !d->pf && (ta == d->latched_ta) &&
                  ((p[2] & 0x07u) == d->es);
        uint8_t st = d->force_status_bad ? 0x00u : 0xAAu;

        if (ok) {
            memcpy(&d->mem[d->latched_ta & (uint16_t)~7u], d->sp31, 8u);
        }
        d->status = st;
        queue_rx(&d->status, 1);
        break;
    }
    default:
        break;
    }
}

static void take_byte(uint8_t b)
{
    if (g_txlen < MOCK_TX_MAX) {
        g_tx[g_txlen++] = b;
    }
    if (!g_parser) {
        return;
    }

    switch (g_ps) {
    case PS_ADDR:
        if (b == 0xF0u) {
            g_search = true;
            g_search_bit = 0;
            memset(g_prefix, 0, sizeof(g_prefix));
        } else if (b == 0xCCu) {
            g_sel = (uint16_t)((1u << g_devs) - 1u);
            g_ps = PS_CMD;
        } else if (b == 0x55u) {
            g_frame_len = 0;
            g_ps = PS_MATCH;
        }
        break;

    case PS_MATCH:
        if (g_frame_len < 8) {
            g_frame[g_frame_len++] = b;
        }
        if (g_frame_len == 8) {
            int i;

            g_sel = 0;
            for (i = 0; i < g_devs; i++) {
                if (memcmp(g_dev[i].rom, g_frame, 8u) == 0) {
                    g_sel |= (uint16_t)(1u << i);
                }
            }
            g_ps = PS_CMD;
        }
        break;

    case PS_CMD: {
        mock_dev_t *d = first_selected();

        g_cmd_strong = g_strong;
        g_cmd = b;
        if (d == 0) {
            g_ps = PS_ADDR;
            break;
        }
        if (d->type == MOCK_DS18B20) {
            execute_cmd(b, 0);
            g_ps = PS_ADDR;
            break;
        }
        switch (b) {
        case 0xF0:
            g_need = 2;
            g_frame_len = 0;
            g_ps = PS_PAYLOAD;
            break;
        case 0x0F:
            g_need = 10;
            g_frame_len = 0;
            g_ps = PS_PAYLOAD;
            break;
        case 0xAA:
            execute_cmd(b, 0);
            g_ps = PS_ADDR;
            break;
        case 0x55:
            g_need = 3;
            g_frame_len = 0;
            g_ps = PS_PAYLOAD;
            break;
        default:
            g_ps = PS_ADDR;
            break;
        }
        break;
    }

    case PS_PAYLOAD:
        if (g_frame_len < (int)sizeof(g_frame)) {
            g_frame[g_frame_len++] = b;
        }
        if (g_frame_len >= g_need) {
            execute_cmd(g_cmd, g_frame);
            g_ps = PS_ADDR;
        }
        break;

    default:
        g_ps = PS_ADDR;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* ow_hal API                                                          */
/* ------------------------------------------------------------------ */

void ow_hal_init(void)
{
    mock_reset();
}

bool ow_hal_busy(void)
{
    return g_active;
}

bool ow_hal_done(void)
{
    if (!g_active) {
        return true;
    }
    if (g_busy_left > 0) {
        g_busy_left--;
        return false;
    }
    g_active = false;
    return true;
}

void ow_hal_feed(const uint8_t *pulses, uint16_t slots)
{
    uint8_t acc = 0;
    int bits = 0;
    int i;

    if ((pulses == 0) || (slots == 0u) || (slots > OW_MAX_SLOTS)) {
        return;
    }

    arm(ARM_PASS);
    g_feed_slots = slots;
    log_ev(MOCK_EV_FEED, slots, pulses[0]);

    if (slots > MOCK_PULSE_MAX - 1) {
        slots = MOCK_PULSE_MAX - 1;
    }
    memcpy(g_pulses, pulses, slots);
    g_pulses[slots] = 0;

    if (!g_parser) {
        for (i = 0; i < slots; i++) {
            acc |= (uint8_t)(((pulses[i] == OW_T_LOW1) ? 1u : 0u) << bits);
            if (++bits == 8) {
                take_byte(acc);
                acc = 0;
                bits = 0;
            }
        }
        return;
    }

    if (g_search && (slots == 1u)) {
        search_direction((pulses[0] == OW_T_LOW1) ? 1 : 0);
        return;
    }

    for (i = 0; i < slots; i++) {
        acc |= (uint8_t)(((pulses[i] == OW_T_LOW1) ? 1u : 0u) << bits);
        if (++bits == 8) {
            take_byte(acc);
            acc = 0;
            bits = 0;
        }
    }
}

static void fill_read(volatile uint8_t *dst, uint16_t slots)
{
    int i;

    for (i = 0; i < (int)slots; i++) {
        if (g_cappos < g_caplen) {
            dst[i] = g_caps[g_cappos++];
        } else if (g_rxpos < (g_rxlen * 8)) {
            int bit = (g_rx[g_rxpos >> 3] >> (g_rxpos & 7)) & 1;
            g_rxpos++;
            dst[i] = (uint8_t)(bit ? 5 : 60);
        } else {
            dst[i] = 5u;
        }
    }
}

void ow_hal_reset(void)
{
    int present;

    arm(ARM_RESET);
    log_ev(MOCK_EV_RESET, 0, 0);

    if (g_presence >= 0) {
        present = g_presence;
    } else {
        present = (g_devs > 0) ? 1 : 0;
    }
    g_reset_caps_now = present ? g_reset_caps : 0;

    g_ps = PS_ADDR;
    g_frame_len = 0;
    g_sel = 0;
    g_search = false;
    g_search_bit = 0;
    memset(g_prefix, 0, sizeof(g_prefix));
    g_rxlen = 0;
    g_rxpos = 0;
    g_mem_stream = false;
    g_mem_ptr = 0;
    g_txlen = 0;
}

void ow_hal_read(volatile uint8_t *dst, uint16_t slots)
{
    if (dst == 0) {
        return;
    }

    if (g_mem_stream) {
        int bytes = (int)((slots + 7u) / 8u);
        int i;
        const mock_dev_t *d = ((g_mem_idx >= 0) && (g_mem_idx < g_devs))
                              ? &g_dev[g_mem_idx] : 0;

        if (bytes > MOCK_RX_MAX) {
            bytes = MOCK_RX_MAX;
        }
        for (i = 0; i < bytes; i++) {
            g_rx[i] = (d != 0) ? d->mem[(g_mem_ptr++) & 1023u] : 0xFFu;
        }
        g_rxlen = bytes;
        g_rxpos = 0;
        g_mem_stream = false;
    }

    fill_read(dst, slots);
    arm(ARM_PASS);
    log_ev(MOCK_EV_READ, slots, 0);
    if (g_strong) {
        g_read_strong = true;
    }
}

void ow_hal_read_pair(volatile uint8_t *dst2)
{
    int id_bit;
    int cmp_bit;

    if (dst2 == 0) {
        return;
    }
    search_pair(&id_bit, &cmp_bit);
    dst2[0] = (uint8_t)(id_bit ? 5 : 60);
    dst2[1] = (uint8_t)(cmp_bit ? 5 : 60);
    arm(ARM_PASS);
    log_ev(MOCK_EV_READ_PAIR, (uint32_t)id_bit, (uint32_t)cmp_bit);
}

void ow_hal_wait(uint32_t us)
{
    int i;

    g_last_wait = (int)us;
    log_ev(MOCK_EV_WAIT, us, 0);

    for (i = 0; i < g_devs; i++) {
        mock_dev_t *d = &g_dev[i];

        if ((d->type == MOCK_DS18B20) && d->converting && (us >= 750000u)) {
            d->sp18[0] = (uint8_t)(d->temp_raw & 0xFF);
            d->sp18[1] = (uint8_t)((uint16_t)d->temp_raw >> 8);
            sp18_refresh(d);
            d->converting = false;
        }
    }

    arm(ARM_PASS);
}

uint16_t ow_hal_captured(uint16_t programmed)
{
    if (g_last_arm == ARM_RESET) {
        int caps = g_reset_caps_now;

        if (caps > (int)programmed) {
            caps = (int)programmed;
        }
        return (uint16_t)caps;
    }
    if (g_read_shortfall >= (int)programmed) {
        return 0u;
    }
    return (uint16_t)(programmed - (uint16_t)g_read_shortfall);
}

void ow_hal_strong_pullup(bool on)
{
    g_strong = on;
    log_ev(MOCK_EV_STRONG, on ? 1u : 0u, 0);
}
