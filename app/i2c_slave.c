#include <stdint.h>

#include "i2c_hw.h"
#include "i2c_latch.h"
#include "i2c_slave.h"
#include "manager.h"
#include "ow_hal.h"

#define STS1_STARTF             0x00000001u
#define STS1_ADDR7F             0x00000002u
#define STS1_STOPF              0x00000010u
#define STS1_RDBF               0x00000040u
#define STS1_TDBE               0x00000080u
#define STS1_BUSERR             0x00000100u
#define STS1_ARLOST             0x00000200u
#define STS1_ACKFAIL            0x00000400u
#define STS1_OUF                0x00000800u
#define STS2_DIRF               0x00000004u
#define SDA_BIT                 0x00000002u
#define SCL_BIT                 0x00000001u

#define STUCK_MS                10u
#define RECOVERY_RETRY_MS       100u
#define RECOVERY_CONFIRM_MS     10u
#define BUS_FRAME_TIMEOUT_MS    1000u

volatile uint32_t g_i2c_err_bus;
volatile uint32_t g_i2c_err_arl;
volatile uint32_t g_i2c_err_ouf;

extern uint8_t g_regs[256];
extern uint32_t pub_phase_max[3];
extern uint32_t pub_phase_sum[3];
extern uint32_t pub_phase_cnt[3];

static uint32_t g_frames;
static uint8_t  g_health;
static uint8_t  g_err_bus;
static volatile uint32_t max_cycles;
static uint8_t  max_state;
static uint16_t max_step_latch;
static uint32_t cycles_per_us = 120u;
static uint32_t g_scl_hz = 100000u;
static uint32_t g_ms;

static uint32_t last_frame_ms;
static uint32_t sda_low_since;
static uint32_t last_recovery_ms;
static bool     sda_low_seen;
static bool     recovery_candidate;
static uint32_t recovery_candidate_ms;

static uint32_t reg_ptr;
static uint32_t rx_mode;
static uint32_t tx_mode;
static const uint8_t *tx_map;
static uint32_t first_byte;
static int      pending_speed = -1;
static bool     speed_apply_pending;
static uint32_t speed_apply_at_cycles;
static uint8_t  pending_cmd;
static uint8_t  dispatch_cmd;
static uint8_t  dispatch_select;
static bool     dispatch_cmd_pending;
static bool     dispatch_select_pending;
static i2c_select_latch_t selected_latch;

/* One-shot fault injection, armed through REG_FAULT. Used by the tester to
 * prove that a corrupted byte, a skipped byte or a miscounted frame is caught
 * instead of silently accepted. */
static uint8_t fault_next;
static uint8_t fault_pending;
static uint8_t fault_drop_frame;
static uint8_t fault_current;
static uint8_t forced_pullup;
static uint8_t fault_edges;
static uint8_t stall_armed;
static bool     fault_unlocked;
static uint32_t fault_unlock_at_cycles;
static uint16_t tx_index;
static uint16_t rx_bytes;

static const uint8_t fault_pulses[65] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    0
};

/* Long-read STOP diagnostic: 0x6C counts STOPs before byte 250; 0x6D stores
 * the last tx_index. */
static uint8_t diag_abort_cnt;
static uint8_t diag_abort_pos;
/* A write transaction is one or two bytes (register pointer, value). More
 * than that in rx_mode means a read address was decoded as a write. */
static uint8_t diag_misdecode;
static uint8_t diag_gate_cnt;
static uint8_t diag_gate_pos;
static uint8_t diag_start_cnt;
static uint8_t diag_bus_cnt;
static uint8_t diag_early_nack_cnt;
static uint8_t diag_early_nack_pos;
static uint8_t diag_hw_abort_count;
static uint8_t diag_hw_abort_pos;
static uint8_t diag_nack_pos;
static uint8_t diag_start_latched;
static uint8_t diag_bus_latched;
static uint8_t diag_ouf_count;
static uint8_t diag_ouf_pos;
static uint8_t diag_ouf_ptr;

static uint8_t sat_inc(uint8_t value)
{
    return (value == 0xFFu) ? value : (uint8_t)(value + 1u);
}

/* Values that live outside the published snapshot: publish() memsets g_regs
 * every 1-Wire cycle, so they are injected when the byte is handed to the
 * master instead of being stored in the map. */
static uint8_t reg_byte(uint32_t addr)
{
    switch (addr & 0xFFu) {
    case REG_FRAMES + 0u:
        return (uint8_t)(g_frames & 0xFFu);
    case REG_FRAMES + 1u:
        return (uint8_t)((g_frames >> 8) & 0xFFu);
    case REG_FRAMES + 2u:
        return (uint8_t)((g_frames >> 16) & 0xFFu);
    case REG_FRAMES + 3u:
        return (uint8_t)((g_frames >> 24) & 0xFFu);
    case REG_HEALTH:
        return g_health;
    case REG_ERR_BUS:
        return g_err_bus;
    case REG_MGR:
        return manager_state_reg();
    case REG_STALL: {
        uint32_t us = pub_phase_max[0] / cycles_per_us;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_STALL_STATE: {
        uint32_t us = pub_phase_max[1] / cycles_per_us;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_STALL_P2: {
        uint32_t us = pub_phase_max[2] / cycles_per_us;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_STALL_AVG: {
        uint32_t us = (pub_phase_cnt[0] != 0u)
                      ? (pub_phase_sum[0] / pub_phase_cnt[0]) / cycles_per_us
                      : 0u;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_STALL_AVG + 1u: {
        uint32_t us = (pub_phase_cnt[1] != 0u)
                      ? (pub_phase_sum[1] / pub_phase_cnt[1]) / cycles_per_us
                      : 0u;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_STALL_AVG + 2u: {
        uint32_t us = (pub_phase_cnt[2] != 0u)
                      ? (pub_phase_sum[2] / pub_phase_cnt[2]) / cycles_per_us
                      : 0u;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_DIAG_CRC_FFFF:
        return diag_abort_cnt;
    case REG_DIAG_BUSY:
        return diag_abort_pos;
    case REG_DIAG_ACK:
        return diag_misdecode;
    case REG_DIAG_REASON:
        return diag_ouf_count;
    case REG_DIAG_GATE:
        return diag_gate_cnt;
    case REG_DIAG_GATE_POS:
        return diag_gate_pos;
    case REG_DIAG_START:
        return diag_start_cnt;
    case REG_DIAG_BUS:
        return diag_bus_cnt;
    case REG_DIAG_EARLY_NACK:
        return diag_early_nack_cnt;
    case REG_DIAG_EARLY_NACK_POS:
        return diag_early_nack_pos;
    case REG_DIAG_ABORT:
        return diag_hw_abort_count;
    case REG_DIAG_ABORT_POS:
        return diag_hw_abort_pos;
    case REG_DIAG_NACK_POS:
        return diag_nack_pos;
    case REG_MAX_STEP: {
        uint32_t us = max_cycles / cycles_per_us;

        max_step_latch = (uint16_t)((us > 0xFFFFu) ? 0xFFFFu : us);
        return (uint8_t)(max_step_latch & 0xFFu);
    }
    case REG_MAX_STEP_H:
        return (uint8_t)(max_step_latch >> 8);
    case REG_MAX_STATE:
        return max_state;
    case REG_OUF_POS:
        return diag_ouf_pos;
    case REG_OUF_PTR:
        return diag_ouf_ptr;
    case REG_EEP_START: {
        uint32_t us = manager_eep_start_max() / cycles_per_us;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    case REG_EEP_POLL: {
        uint32_t us = manager_eep_poll_max() / cycles_per_us;

        return (uint8_t)((us > 255u) ? 255u : us);
    }
    default:
        if ((addr == REG_SPEED) ||
            ((addr >= REG_CMD) && (addr <= REG_FAULT))) {
            /* Host-owned bytes: written by the master, never by a publish. */
            return g_regs[addr & 0xFFu];
        }
        return tx_map[addr & 0xFFu];
    }
}

uint8_t i2c_slave_reg_byte(uint32_t addr)
{
    return reg_byte(addr);
}

static void health_clear(void)
{
    g_frames = 0u;
    g_health = 0u;
    g_err_bus = 0u;
    g_i2c_err_bus = 0u;
    g_i2c_err_arl = 0u;
    g_i2c_err_ouf = 0u;
    fault_next = 0u;
    fault_pending = 0u;
    fault_drop_frame = 0u;
    fault_current = 0u;
    if (forced_pullup != 0u) {
        forced_pullup = 0u;
        ow_hal_strong_pullup(false);
    }
    fault_edges = 0u;
    stall_armed = 0u;
    fault_unlocked = false;
    fault_unlock_at_cycles = 0u;
    tx_index = 0u;
    rx_bytes = 0u;
    diag_misdecode = 0u;
    diag_abort_cnt = 0u;
    diag_abort_pos = 0u;
    diag_ouf_count = 0u;
    diag_ouf_pos = 0u;
    diag_ouf_ptr = 0u;
    diag_gate_cnt = 0u;
    diag_gate_pos = 0u;
    diag_start_cnt = 0u;
    diag_bus_cnt = 0u;
    diag_early_nack_cnt = 0u;
    diag_early_nack_pos = 0u;
    diag_hw_abort_count = 0u;
    diag_hw_abort_pos = 0u;
    diag_nack_pos = 0u;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
}

static void fault_apply(uint8_t code)
{
    switch (code) {
    case FAULT_EXTRA_FRAME:
        g_frames++;
        break;
    case FAULT_DROP_FRAME:
        fault_drop_frame = 1u;
        break;
    case FAULT_BIT_FLIP:
    case FAULT_SKIP_BYTE:
        fault_pending = code;
        break;
    case FAULT_CURRENT:
        fault_current = 1u;
        break;
    case FAULT_EDGES:
        fault_edges = 1u;
        break;
    case FAULT_STALL:
        stall_armed = 1u;
        break;
    case FAULT_ABORT:
        fault_pending = code;
        break;
    default:
        break;
    }
}

bool i2c_slave_busy(void)
{
    /* STS2.busyf is not usable here: it is set on every START and this slave
     * has been seen to keep it set after a mangled frame, which starved
     * manager_poll(). The software frame view is exact. */
    return (tx_mode != 0u) || (rx_mode != 0u);
}

static void i2c_speed_idle_poll(void)
{
    if (!speed_apply_pending ||
        ((int32_t)(i2c_hw_cycles() - speed_apply_at_cycles) < 0) ||
        i2c_hw_busy() ||
        ((i2c_hw_gpio_idt() & (SDA_BIT | SCL_BIT)) != (SDA_BIT | SCL_BIT))) {
        return;
    }
    i2c_hw_apply_speed(pending_speed != 0 ? 400000u : 100000u);
    g_scl_hz = (pending_speed != 0) ? 400000u : 100000u;
    g_i2c_speed = (pending_speed != 0) ? 1u : 0u;
    pending_speed = -1;
    speed_apply_pending = false;
}

static void i2c_abort_frame(void);

/* Hand one snapshot byte to the master, applying a one-shot injected fault.
 * Both the address-match byte and the TDBE byte go through here. */
static void i2c_send_byte(void)
{
    uint8_t value = reg_byte(reg_ptr);

    if ((stall_armed != 0u) && (tx_index == 64u)) {
        uint32_t us = (uint32_t)(g_regs[REG_SELECTED] & 0x0Fu) * 20u;

        stall_armed = 0u;
        i2c_hw_delay_us((us == 0u) ? 20u : us);
    }
    tx_index++;

    if (fault_next != 0u) {
        uint8_t fault = fault_next;

        fault_next = 0u;
        if (fault == FAULT_ABORT) {
            i2c_abort_frame();
            return;
        }
        if (fault == FAULT_BIT_FLIP) {
            value ^= 0x01u;
        } else if (fault == FAULT_SKIP_BYTE) {
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
            value = reg_byte(reg_ptr);
        }
    }
    i2c_hw_dt_write(value);
    reg_ptr = (reg_ptr + 1u) & 0xFFu;
}

/* Finish the previous transaction. Called from STOPF and from ADDR7F when a
 * new transaction starts before the previous STOP was serviced. */
static void i2c_stop_done(void)
{
    if ((tx_mode != 0u) && (tx_index >= 16u) && (tx_index < 250u)) {
        if (diag_abort_cnt != 0xFFu) {
            diag_abort_cnt++;
        }
        diag_abort_pos = (uint8_t)tx_index;
    }
    (void)i2c_hw_sts1();
    i2c_hw_stop_rearm();
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;

    if (forced_pullup != 0u) {
        forced_pullup = 0u;
        ow_hal_strong_pullup(false);
    }
    if (fault_pending != 0u) {
        fault_next = fault_pending;
        fault_pending = 0u;
    }
    if (pending_cmd != 0u) {
        dispatch_cmd = pending_cmd;
        dispatch_cmd_pending = true;
        pending_cmd = 0u;
    }
    if (pending_speed >= 0) {
        /* Reinitialising I2C here can race the next START. The idle poll
         * applies the change after a short quiet window. */
        speed_apply_pending = true;
        speed_apply_at_cycles = i2c_hw_cycles() + (50u * cycles_per_us);
    }
    if (i2c_select_latch_commit(&selected_latch, &dispatch_select)) {
        dispatch_select_pending = true;
    }
}

void i2c_slave_dispatch_pending(void)
{
    if (i2c_slave_busy()) {
        return;
    }
    if (dispatch_cmd_pending) {
        uint8_t cmd = dispatch_cmd;

        dispatch_cmd_pending = false;
        if (cmd == 3u) {
            health_clear();
        } else if (cmd == 7u) {
            max_cycles = 0u;
            max_state = 0u;
            pub_phase_max[0] = 0u;
            pub_phase_max[1] = 0u;
            pub_phase_max[2] = 0u;
            pub_phase_sum[0] = 0u;
            pub_phase_sum[1] = 0u;
            pub_phase_sum[2] = 0u;
            pub_phase_cnt[0] = 0u;
            pub_phase_cnt[1] = 0u;
            pub_phase_cnt[2] = 0u;
        }
        manager_command(cmd);
    }
    if (dispatch_select_pending) {
        dispatch_select_pending = false;
        manager_select(dispatch_select);
    }
}

static void i2c_abort_frame(void)
{
    if (diag_hw_abort_count != 0xFFu) {
        diag_hw_abort_count++;
    }
    diag_hw_abort_pos = (uint8_t)tx_index;
    i2c_hw_abort();

    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
    dispatch_cmd_pending = false;
    dispatch_select_pending = false;
    pending_speed = -1;
    speed_apply_pending = false;
    i2c_select_latch_abort(&selected_latch);
    fault_unlocked = false;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
}

static void i2c_poll_frame(void)
{
    uint32_t sts1;
    uint32_t relevant;

    sts1 = i2c_hw_sts1();
    relevant = sts1 & ~STS1_TDBE;

    /* STARTF and BUSERR are diagnostic only here. Count each flag once per
     * software frame; the normal handlers below clear them. */
    if ((tx_mode != 0u) || (rx_mode != 0u)) {
        if (((sts1 & STS1_STARTF) != 0u) && !diag_start_latched) {
            diag_start_cnt = sat_inc(diag_start_cnt);
            diag_start_latched = 1u;
        }
        if (((sts1 & STS1_BUSERR) != 0u) && !diag_bus_latched) {
            diag_bus_cnt = sat_inc(diag_bus_cnt);
            diag_bus_latched = 1u;
        }
    }

    /* An underrun has to be handled before another byte is served, otherwise
     * the recorded position belongs to a later frame. */
    if ((sts1 & STS1_OUF) != 0u) {
        diag_ouf_pos = (uint8_t)tx_index;
        diag_ouf_ptr = (uint8_t)reg_ptr;
        if (diag_ouf_count != 0xFFu) {
            diag_ouf_count++;
        }
        g_i2c_err_ouf++;
        g_health |= HEALTH_OUF;
        i2c_hw_clear_flags(STS1_OUF);
        i2c_abort_frame();
        return;
    }

    if ((sts1 & STS1_ACKFAIL) != 0u) {
        if ((tx_index > 16u) && (tx_index < 255u)) {
            diag_nack_pos = (uint8_t)tx_index;
        }
        /* ACKFAIL alone is not proof that the master stopped reading; retain
         * tx_mode until STOPF or a new address match retires this frame. Full
         * 256-byte reads end at 256, and tester reads are at most 128 bytes. */
        if ((tx_mode != 0u) && (tx_index > 128u) && (tx_index < 255u)) {
            if (diag_early_nack_cnt != 0xFFu) {
                diag_early_nack_cnt++;
            }
            diag_early_nack_pos = (uint8_t)tx_index;
        }
        i2c_hw_clear_flags(STS1_ACKFAIL);
    }

    if ((relevant == 0u) && (tx_mode == 0u)) {
        return;
    }

    if ((sts1 & STS1_ADDR7F) != 0u) {
        uint32_t sts2;

        /* A STOP from the previous transaction may still be pending if the
         * master did not wait for it; finish it before starting the new one. */
        if ((sts1 & STS1_STOPF) != 0u) {
            i2c_stop_done();
        }

        (void)i2c_hw_sts1();
        sts2 = i2c_hw_sts2();
        /* Retire flags left by the previous frame. BUSERR and ARLOST are
         * serviced here because the address branch returns before the normal
         * error handler. ACKFAIL was already handled at the top. */
        {
            uint32_t stale = sts1 & (STS1_ACKFAIL | STS1_BUSERR | STS1_ARLOST);

            if ((stale & STS1_BUSERR) != 0u) {
                g_i2c_err_bus++;
                g_err_bus = sat_inc(g_err_bus);
                g_health |= HEALTH_BUSERR;
            }
            if ((stale & STS1_ARLOST) != 0u) {
                g_i2c_err_arl++;
                g_health |= HEALTH_ARLOST;
            }
            if (stale != 0u) {
                i2c_hw_clear_flags(stale);
            }
        }
        /* Direction was sampled immediately after the STS1 read that clears
         * ADDRF. Do not move the STS2 read below the stale-flag writes. */
        g_frames++;
#ifndef DIAG_BUILD
        last_frame_ms = g_ms;
#endif
        if (fault_drop_frame != 0u) {
            g_frames--;
            fault_drop_frame = 0u;
        }
        diag_start_latched = 0u;
        diag_bus_latched = 0u;
        /* A new address starts before the idle dispatcher can run. Commands
         * and selections belong to the previous transaction and are dropped. */
        dispatch_cmd_pending = false;
        dispatch_select_pending = false;
        i2c_select_latch_abort(&selected_latch);
        if ((sts2 & STS2_DIRF) != 0u) {
            tx_mode = 1;
            rx_mode = 0;
            tx_index = 0u;
            tx_map = manager_map();
            i2c_send_byte();
        } else {
            rx_mode = 1;
            tx_mode = 0;
            rx_bytes = 0u;
            first_byte = 1;
        }

        if (fault_current != 0u) {
            fault_current = 0u;
            forced_pullup = 1u;
            ow_hal_strong_pullup(true);
        }
        if (fault_edges != 0u) {
            fault_edges = 0u;
            ow_hal_feed(fault_pulses, 64u);
        }
        return;
    }

    /* A broken frame must not clock another byte or accept another register
     * byte. Clear the error, release any hardware stretch and drop the frame. */
    if ((sts1 & (STS1_BUSERR | STS1_ARLOST)) != 0u) {
        if ((sts1 & STS1_BUSERR) != 0u) {
            g_i2c_err_bus++;
            g_err_bus = sat_inc(g_err_bus);
            g_health |= HEALTH_BUSERR;
        }
        if ((sts1 & STS1_ARLOST) != 0u) {
            g_i2c_err_arl++;
            g_health |= HEALTH_ARLOST;
        }
        i2c_hw_clear_flags(sts1 & (STS1_BUSERR | STS1_ARLOST));
        i2c_abort_frame();
        return;
    }

    /* A STOP is only acted on when the bus is released, otherwise a spurious
     * flag would cut a live transmit in half. ADDR7F above retires the old
     * frame unconditionally because the next transaction has already started. */
    if ((sts1 & STS1_STOPF) != 0u) {
        if (((i2c_hw_gpio_idt() & (SDA_BIT | SCL_BIT)) == (SDA_BIT | SCL_BIT)) ||
            (tx_mode == 0u)) {
            i2c_stop_done();
        } else {
            if (diag_gate_cnt != 0xFFu) {
                diag_gate_cnt++;
            }
            diag_gate_pos = (uint8_t)tx_index;
        }
        return;
    }

    if ((rx_mode != 0u) && ((sts1 & STS1_RDBF) != 0u)) {
        uint8_t value = i2c_hw_dt_read();

        if (rx_bytes != 0xFFFFu) {
            rx_bytes++;
        }
        if ((rx_bytes == 9u) && (diag_misdecode != 0xFFu)) {
            diag_misdecode++;
        }

        if (first_byte != 0u) {
            reg_ptr = value;
            first_byte = 0;
        } else {
            /* Only REG_SPEED and REG_CMD are writable. Everything else is a
             * read-only snapshot; the pointer still advances on block writes. */
            if (reg_ptr == REG_SPEED) {
                if (value <= 1u) {
                    g_regs[REG_SPEED] = value;
                    pending_speed = (int)value;
                }
            } else if (reg_ptr == REG_CMD) {
                g_regs[REG_CMD] = value;
                pending_cmd = value;
            } else if (reg_ptr == REG_SELECTED) {
                i2c_select_latch_write(&selected_latch, value);
            } else if (reg_ptr == REG_FAULT) {
                if (value == FAULT_UNLOCK_KEY) {
                    fault_unlocked = true;
                    fault_unlock_at_cycles = i2c_hw_cycles();
                } else {
                    uint32_t elapsed = i2c_hw_cycles() - fault_unlock_at_cycles;

                    if (fault_unlocked &&
                        (elapsed <= (FAULT_UNLOCK_WINDOW_US * cycles_per_us))) {
                        fault_apply(value);
                    }
                    fault_unlocked = false;
                }
            }
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        }
        return;
    }

    if ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) {
        i2c_send_byte();
    }
}

void i2c_slave_poll(void)
{
    i2c_poll_frame();
    if (!i2c_slave_busy()) {
        i2c_speed_idle_poll();
    }
}

void i2c_slave_health_poll(uint32_t now_ms)
{
    uint32_t sts1 = i2c_hw_sts1();

    g_ms = now_ms;
    if ((now_ms - last_frame_ms) < STUCK_MS) {
        sda_low_seen = false;
        return;
    }
    /* Do not mistake a master pause between bytes for stuck SDA. When the
     * slave is waiting for received data, or still has data queued for transmit,
     * static SDA/SCL levels are a legal paused frame. */
    if ((rx_mode && ((sts1 & STS1_RDBF) == 0u)) ||
        (tx_mode && ((sts1 & STS1_TDBE) == 0u))) {
        sda_low_seen = false;
        return;
    }
    if ((i2c_hw_gpio_idt() & SDA_BIT) != 0u) {
        sda_low_seen = false;
        return;
    }
    if (!sda_low_seen) {
        sda_low_seen = true;
        sda_low_since = now_ms;
        return;
    }
    if ((i2c_hw_gpio_idt() & SCL_BIT) == 0u) {
        sda_low_seen = false;
        return;
    }
    if ((now_ms - sda_low_since) < STUCK_MS) {
        return;
    }
    if ((now_ms - last_recovery_ms) < RECOVERY_RETRY_MS) {
        return;
    }
    g_health |= HEALTH_SDA_STUCK;
    i2c_slave_recover(now_ms);
}

static bool frame_owes_byte(uint32_t sts1)
{
    return ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) ||
           ((rx_mode != 0u) && ((sts1 & STS1_RDBF) != 0u));
}

bool i2c_slave_recovery_due(uint32_t now_ms)
{
    uint32_t sts1;
    uint32_t gpio;

    if (!i2c_slave_busy() || ((now_ms - last_frame_ms) <= BUS_FRAME_TIMEOUT_MS)) {
        recovery_candidate = false;
        return false;
    }
    sts1 = i2c_hw_sts1();
    if ((sts1 & (STS1_STARTF | STS1_ADDR7F)) != 0u) {
        recovery_candidate = false;
        return false;
    }
    gpio = i2c_hw_gpio_idt();
    /* SDA low with SCL high can be a live START/STOP or the dedicated
     * SDA-stuck case. Do not race it with open-frame recovery. */
    if ((gpio & SDA_BIT) == 0u) {
        recovery_candidate = false;
        return false;
    }
    /* A low SCL with no byte owed belongs to a master that may still be
     * clocking or paused. Only a hardware-stretched slave may recover it. */
    if (((gpio & SCL_BIT) == 0u) && !frame_owes_byte(sts1)) {
        recovery_candidate = false;
        return false;
    }
    /* GPIO can go idle before STOPF reaches the peripheral. Require a short
     * unchanged quiet window so the normal STOP/ADDR handler wins the race. */
    if (!recovery_candidate) {
        recovery_candidate = true;
        recovery_candidate_ms = now_ms;
        return false;
    }
    return (now_ms - recovery_candidate_ms) >= RECOVERY_CONFIRM_MS;
}

void i2c_slave_recover(uint32_t now_ms)
{
    uint32_t sts1 = i2c_hw_sts1();
    i2c_recovery_result_t result =
        i2c_hw_recover(g_scl_hz, frame_owes_byte(sts1));

    if (result == I2C_RECOVERY_DEFERRED) {
        return;
    }
    recovery_candidate = false;

    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
    dispatch_cmd_pending = false;
    dispatch_select_pending = false;
    pending_speed = -1;
    speed_apply_pending = false;
    i2c_select_latch_abort(&selected_latch);
    fault_next = 0u;
    fault_pending = 0u;
    fault_drop_frame = 0u;
    fault_current = 0u;
    if (forced_pullup != 0u) {
        forced_pullup = 0u;
        ow_hal_strong_pullup(false);
    }
    fault_edges = 0u;
    stall_armed = 0u;
    fault_unlocked = false;
    fault_unlock_at_cycles = 0u;
    tx_index = 0u;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
    sda_low_seen = false;
    last_recovery_ms = now_ms;
    last_frame_ms = now_ms;

    if (result == I2C_RECOVERY_OK) {
        g_health |= HEALTH_RECOVERED;
    } else {
        g_health |= HEALTH_RECOVERY_FAILED;
    }
}

void i2c_slave_note_manager_poll(uint32_t cycles, uint8_t state)
{
    if (cycles > max_cycles) {
        max_cycles = cycles;
        max_state = state;
    }
}

void i2c_slave_set_cycles_per_us(uint32_t cycles)
{
    if (cycles != 0u) {
        cycles_per_us = cycles;
    }
}

void i2c_slave_init(uint32_t reset_cause)
{
    health_clear();
    g_health |= reset_cause & (HEALTH_WDT_RESET | HEALTH_SW_RESET);
    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
    dispatch_cmd = 0u;
    dispatch_select = 0u;
    dispatch_cmd_pending = false;
    dispatch_select_pending = false;
    pending_speed = -1;
    speed_apply_pending = false;
    i2c_select_latch_abort(&selected_latch);
    tx_map = manager_map();
    g_scl_hz = 100000u;
    g_i2c_speed = 0u;
    last_frame_ms = 0u;
    sda_low_since = 0u;
    last_recovery_ms = 0u;
    sda_low_seen = false;
    recovery_candidate = false;
    recovery_candidate_ms = 0u;
}
