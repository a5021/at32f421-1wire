#include <stdint.h>
#include <string.h>

#include "at32f421.h"
#include "clock.h"
#include "i2c_latch.h"
#include "manager.h"
#include "ow_hal.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds2431.h"

#define I2C_ADDR7               0x42u
#define I2C_OADDR1              ((uint16_t)(I2C_ADDR7 << 1))

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

#define WDT_CMD_UNLOCK         0x00005555u
#define WDT_CMD_ENABLE         0x0000CCCCu
#define WDT_CMD_RELOAD         0x0000AAAAu
#define WDT_DIV_64             0x00000004u
#define WDT_RELOAD_4S          2500u
#define WDT_CMD                (*(volatile uint32_t *)(WDT_BASE + 0x00u))
#define WDT_DIV                (*(volatile uint32_t *)(WDT_BASE + 0x04u))
#define WDT_RLD                (*(volatile uint32_t *)(WDT_BASE + 0x08u))
#define WDT_STS                (*(volatile uint32_t *)(WDT_BASE + 0x0Cu))

extern volatile uint32_t g_i2c_speed;

extern uint8_t g_regs[256];
extern uint32_t pub_phase_max[3];
static uint8_t diag_ouf_count;
static uint8_t diag_ouf_pos;
static uint8_t diag_ouf_ptr;
static uint16_t max_step_latch;     /* us, latched so the two bytes agree */
extern uint32_t pub_phase_sum[3];
extern uint32_t pub_phase_cnt[3];

static uint32_t reg_ptr;
static uint32_t rx_mode;
static uint32_t tx_mode;
static const uint8_t *tx_map;       /* map snapshot latched at the address match */
static uint32_t first_byte;
static int      pending_speed = -1;
static bool     speed_apply_pending;
static uint32_t speed_apply_at_cycles;
static uint8_t  pending_cmd;
static i2c_select_latch_t selected_latch;

/* One-shot fault injection, armed through REG_FAULT.  Used by the tester to
 * prove that a corrupted byte, a skipped byte or a miscounted frame is caught
 * instead of silently accepted. */
static uint8_t fault_next;          /* armed for the next transmitted byte */
static uint8_t fault_pending;       /* written during the current transaction */
static uint8_t fault_drop_frame;
static uint8_t fault_current;       /* strong pull-up during the next frame */
static uint8_t forced_pullup;       /* we turned the strong pull-up on */
static uint8_t fault_edges;         /* 1-Wire burst during the next frame */
static uint8_t stall_armed;         /* stall once at byte 64 of the read */
static uint16_t tx_index;           /* transmitted bytes in this frame */

static const uint8_t fault_pulses[65] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    0
};

volatile uint32_t g_i2c_err_bus;
volatile uint32_t g_i2c_err_arl;
volatile uint32_t g_i2c_err_ouf;

/* Bus health, readable over I2C without a debugger.
 *
 * The slave cannot see a master NACK: if the address phase is corrupted the
 * address simply does not match and no flag is raised.  What it can count is
 * matched addresses, so the master compares its own transaction count with
 * this counter and the difference is exactly the address-phase loss rate.
 *
 * The counter only changes at an address match, which happens before the
 * master reads it, so a four-byte read is consistent without a seqlock. */
static uint32_t g_frames;           /* matched addresses */
static uint8_t  g_health;           /* HEALTH_* sticky bits */
static uint8_t  g_err_bus;          /* saturated BUSERR count */
static volatile uint32_t max_cycles;/* worst manager_poll duration */
static uint8_t  max_state;          /* state during that call */
static uint32_t cycles_per_us = 120u;   /* measured at boot */

static uint32_t g_ms;               /* 1 ms tick, polled from SysTick */
static uint32_t g_scl_hz = 100000u; /* timing currently programmed */

#define SDA_BIT                 0x00000002u
#define SCL_BIT                 0x00000001u

#ifndef DIAG_BUILD
static uint32_t last_frame_ms;
static uint32_t sda_low_since;
static uint32_t last_recovery_ms;
static bool     sda_low_seen;
#define STUCK_MS                10u
#define RECOVERY_RETRY_MS       100u
#define BUS_FRAME_TIMEOUT_MS    250u
#endif

/* Long-read STOP diagnostic: 0x6C counts STOPs before byte 250; 0x6D stores
 * the last tx_index. */
static uint8_t  diag_abort_cnt;
static uint8_t  diag_abort_pos;     /* tx_index of the last aborted frame */
/* A write transaction is one or two bytes (register pointer, value).  More
 * than that in rx_mode means a read address was decoded as a write: the slave
 * then ACKs and drives nothing, so the master reads the whole block as 0xFF
 * with no I2C error anywhere. */
static uint16_t rx_bytes;
static uint8_t  diag_misdecode;
static uint8_t  diag_gate_cnt;      /* STOPF refused while tx_mode was set */
static uint8_t  diag_gate_pos;      /* tx_index at the last refused STOPF */
static uint8_t  diag_start_cnt;     /* STARTF seen while a frame was open */
static uint8_t  diag_bus_cnt;       /* BUSERR seen while a frame was open */
static uint8_t  diag_early_nack_cnt;
static uint8_t  diag_early_nack_pos;
static uint8_t  diag_start_latched;
static uint8_t  diag_bus_latched;

static uint8_t sat_inc(uint8_t v)
{
    return (v == 0xFFu) ? v : (uint8_t)(v + 1u);
}

static void timebase_init(void)
{
    /* HCLK/8 counts a millisecond without any interrupt; the main loop reads
     * COUNTFLAG, so a long blocking pass only delays detection, never advances
     * the clock early. */
    SysTick->LOAD = (clock_ahb_hz() / 8000u) - 1u;
    SysTick->VAL = 0u;
    SysTick->CTRL = 0x00000001u;    /* HCLK/8, enable, no tick interrupt */

    /* Cycle counter for measuring how long the manager blocks the loop. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void timebase_poll(void)
{
    if ((SysTick->CTRL & 0x00010000u) != 0u) {
        g_ms++;
    }
}

#ifndef DIAG_BUILD
static void watchdog_init(void)
{
    uint32_t guard = 100000u;

    /* LICK is 30..60 kHz (40 kHz typical). /64 and 2500 ticks give a
     * nominal 4 s timeout, bounded by about 2.7..5.3 s across that range. */
    CRM->ctrlsts_bit.licken = 1u;
    while ((CRM->ctrlsts_bit.lickstbl == 0u) && (guard != 0u)) {
        guard--;
    }
    WDT_CMD = WDT_CMD_UNLOCK;
    WDT_DIV = WDT_DIV_64;
    WDT_RLD = WDT_RELOAD_4S;
    while (((WDT_STS & 0x00000003u) != 0x00000003u) && (guard != 0u)) {
        guard--;
    }
    WDT_CMD = WDT_CMD_ENABLE;
    WDT_CMD = WDT_CMD_RELOAD;
}

static void watchdog_kick(void)
{
    WDT_CMD = WDT_CMD_RELOAD;
}
#endif

/* Measure how many DWT cycles a real millisecond takes, so the diagnostics do
 * not depend on the nominal clock. */
static void calibrate_cycles(void)
{
    uint32_t m0 = g_ms;
    uint32_t t0 = DWT->CYCCNT;

    while ((g_ms - m0) < 50u) {
        timebase_poll();
    }

    uint32_t c = DWT->CYCCNT - t0;

    if ((c > 5000u) && (c < 12000000u)) {
        cycles_per_us = c / 50000u;
        if (cycles_per_us == 0u) {
            cycles_per_us = 1u;
        }
    }
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
        ow_hal_strong_pullup(false);    /* never leave PA10 driving high */
    }
    fault_edges = 0u;
    stall_armed = 0u;
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
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
}

static void fault_apply(uint8_t code)
{
    switch (code) {
    case FAULT_EXTRA_FRAME:
        g_frames++;                 /* phantom frame: master must clamp */
        break;
    case FAULT_DROP_FRAME:
        fault_drop_frame = 1u;      /* next matched address is not counted */
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
    default:
        break;
    }
}

bool manager_i2c_busy(void)
{
    /* The peripheral's STS2.busyf is not usable here: it is set on every START
     * and this slave has been seen to keep it set after a mangled frame, which
     * then starved manager_poll() for minutes (measured: the cycle counter
     * froze while the manager sat in MGR_CONVERT).  The software frame view is
     * exact: it is armed at the address match and retired at the STOP. */
    return (tx_mode != 0u) || (rx_mode != 0u);
}

static bool i2c_active(void)
{
    return manager_i2c_busy();
}

/* Program the slave clocking from the APB1 frequency that is actually running.
 * speed is the SCL prescaler: APB1 / (2 * SCL) in standard mode and
 * APB1 / (3 * SCL) for fast mode with a 1:2 high:low ratio, which is what the
 * reference manual and the vendor BSP use. */
static void i2c_apply_speed(uint32_t scl_hz)
{
    uint32_t apb1 = clock_apb1_hz();
    uint32_t mhz = apb1 / 1000000u;
    uint32_t speed;
    uint32_t rise;

    I2C2->ctrl1_bit.i2cen = 0;
    I2C2->ctrl2_bit.clkfreq = mhz;

    if (scl_hz <= 100000u) {
        speed = apb1 / (2u * scl_hz);
        if (speed < 4u) {
            speed = 4u;
        }
        I2C2->clkctrl_bit.speedmode = 0;
        rise = mhz + 1u;
    } else {
        speed = apb1 / (3u * scl_hz);
        if (speed == 0u) {
            speed = 1u;
        }
        I2C2->clkctrl_bit.speedmode = 1;
        rise = (mhz * 300u) / 1000u + 1u;
    }

    if (speed > 0xFFFu) {
        speed = 0xFFFu;
    }
    if (rise > 0x3Fu) {
        /* 100 kHz at 120 MHz APB1 asks for 121, but TRISE is 6 bits. The
         * clamped 64 ticks are about 533 ns, still inside the 1000 ns standard
         * mode maximum, so this is a field-width limit, not a timing failure. */
        rise = 0x3Fu;
    }

    I2C2->clkctrl_bit.dutymode = 0;             /* 1:2 high:low in fast mode */
    I2C2->clkctrl_bit.speed = speed;
    I2C2->tmrise_bit.risetime = rise;

    /* CTRL1 bit 7 is NOSTRETCH, not "enable stretching": 0 holds SCL low while
     * DT is being loaded, 1 lets the master clock the byte out immediately.
     * Program it while the peripheral is disabled. */
    I2C2->ctrl1_bit.stretch = 0;
    I2C2->ctrl1_bit.i2cen = 1;
    I2C2->ctrl1_bit.acken = 1;

    g_i2c_speed = (scl_hz > 100000u) ? 1u : 0u;
    g_scl_hz = scl_hz;
}

static void i2c_hw_init(void)
{
    CRM->ahben_bit.gpioaen = 1;
    CRM->apb1en_bit.i2c2en = 1;

    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x0000000Au;
    GPIOA->omode |= 0x00000003u;
    GPIOA->pull &= ~0x0000000Fu;
    GPIOA->muxl = (GPIOA->muxl & ~0x000000FFu) | 0x00000044u;

    I2C2->ctrl1 = 0;
    I2C2->ctrl2 = 0;
    I2C2->oaddr1 = I2C_OADDR1;
    I2C2->oaddr1_bit.addr1mode = 0;

    i2c_apply_speed(g_scl_hz);
}

static void spin_us(uint32_t us)
{
    while (us-- != 0u) {
        for (volatile uint32_t i = 0u; i < 60u; i++) {
        }
    }
}

#ifndef DIAG_BUILD
/* Nine clocks and a STOP free a slave left mid-byte. The caller must prove
 * the master stopped talking: either no address arrived for the open-frame
 * timeout, or SDA has been stuck low with SCL high for STUCK_MS. */
static bool i2c_bus_recover(void)
{
    bool scl_ok = true;
    bool recovered;

    I2C2->ctrl1_bit.i2cen = 0;
    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x00000005u;
    GPIOA->omode |= 0x00000003u;
    GPIOA->odt |= (SDA_BIT | SCL_BIT);
    spin_us(5u);

    {
        for (int i = 0; i < 9; i++) {
            GPIOA->odt &= ~SCL_BIT;
            spin_us(5u);
            GPIOA->odt |= SCL_BIT;
            spin_us(5u);
            if ((GPIOA->idt & SCL_BIT) == 0u) {
                scl_ok = false;
                break;
            }
        }
        if (scl_ok) {
            GPIOA->odt &= ~SDA_BIT;
            spin_us(5u);
            GPIOA->odt |= SCL_BIT;
            spin_us(5u);
            GPIOA->odt |= SDA_BIT;
            spin_us(5u);
        }
    }

    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x0000000Au;
    GPIOA->omode |= 0x00000003u;
    i2c_hw_init();

    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
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
    tx_index = 0u;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
    sda_low_seen = false;
    last_recovery_ms = g_ms;
    last_frame_ms = g_ms;

    recovered = scl_ok &&
                ((GPIOA->idt & (SDA_BIT | SCL_BIT)) == (SDA_BIT | SCL_BIT));
    if (recovered) {
        g_health |= HEALTH_RECOVERED;
    } else {
        g_health |= HEALTH_RECOVERY_FAILED;
    }
    return recovered;
}

static void bus_health_poll(void)
{
    uint32_t sts1 = I2C2->sts1;

    if ((g_ms - last_frame_ms) < STUCK_MS) {
        sda_low_seen = false;
        return;
    }
    /* Do not mistake a master pause between bytes for stuck SDA. When the
     * slave is waiting for received data, or still has data queued for transmit,
     * static SDA/SCL levels are a legal paused frame. The dangerous case is the
     * peripheral owing a byte (TDBE/RDBF set) while SCL is nevertheless high. */
    if ((rx_mode && ((sts1 & STS1_RDBF) == 0u)) ||
        (tx_mode && ((sts1 & STS1_TDBE) == 0u))) {
        sda_low_seen = false;
        return;
    }
    if ((GPIOA->idt & SDA_BIT) != 0u) {
        sda_low_seen = false;
        return;
    }
    if (!sda_low_seen) {
        sda_low_seen = true;
        sda_low_since = g_ms;
        return;
    }
    if ((GPIOA->idt & SCL_BIT) == 0u) {
        sda_low_seen = false;
        return;
    }
    if ((g_ms - sda_low_since) < STUCK_MS) {
        return;
    }
    if ((g_ms - last_recovery_ms) < RECOVERY_RETRY_MS) {
        return;
    }
    g_health |= HEALTH_SDA_STUCK;
    i2c_bus_recover();
}
#endif

/* Hand one snapshot byte to the master, applying a one-shot injected fault.
 * Both the address-match byte and the TDBE byte go through here, otherwise a
 * one-byte read would never see the fault. */
static void i2c_send_byte(void)
{
    uint8_t v = reg_byte(reg_ptr);

    if ((stall_armed != 0u) && (tx_index == 64u)) {
        uint32_t us = (uint32_t)(g_regs[REG_SELECTED] & 0x0Fu) * 20u;

        stall_armed = 0u;
        spin_us((us == 0u) ? 20u : us);  /* stall length from REG_SELECTED */
    }
    tx_index++;

    if (fault_next != 0u) {
        uint8_t f = fault_next;

        fault_next = 0u;
        if (f == FAULT_BIT_FLIP) {
            v ^= 0x01u;
        } else if (f == FAULT_SKIP_BYTE) {
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
            v = reg_byte(reg_ptr);
        }
    }
    I2C2->dt = v;
    reg_ptr = (reg_ptr + 1u) & 0xFFu;
}

/* Finish the previous transaction.  Called from the STOPF branch and, if the
 * master starts a new transaction before the STOP was serviced, at the next
 * address match: a stale STOP must never apply a command to the wrong
 * transaction. */
static void i2c_stop_done(void){
    /* Long map reads end at byte 256. A STOP between bytes 16 and 249 is an
     * early termination. Do not gate this diagnostic on ACKFAIL: ACKFAIL can
     * be stale or sampled late while this slave is being clocked. */
    if ((tx_mode != 0u) && (tx_index >= 16u) && (tx_index < 250u)) {
        if (diag_abort_cnt != 0xFFu) {
            diag_abort_cnt++;
        }
        diag_abort_pos = (uint8_t)tx_index;
    }
    (void)I2C2->sts1;
    I2C2->ctrl1_bit.i2cen = 1;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    diag_start_latched = 0u;
    diag_bus_latched = 0u;

    if (forced_pullup != 0u) {
        forced_pullup = 0u;
        ow_hal_strong_pullup(false);    /* release only what we forced */
    }
    if (fault_pending != 0u) {
        fault_next = fault_pending;     /* one shot, next transaction */
        fault_pending = 0u;
    }
    if (pending_cmd != 0u) {
        if (pending_cmd == 3u) {
            health_clear();
        } else if (pending_cmd == 7u) {
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
        manager_command(pending_cmd);
        pending_cmd = 0u;
    }
    if (pending_speed >= 0) {
        /* Reinitialising I2C here can race the next START. The main loop
         * applies the change after a short idle window. */
        speed_apply_pending = true;
        speed_apply_at_cycles = DWT->CYCCNT + (50u * cycles_per_us);
    }
    {
        uint8_t selected;

        if (i2c_select_latch_commit(&selected_latch, &selected)) {
            manager_select(selected);
        }
    }
}

static void i2c_abort_frame(void)
{
    /* A failed frame can leave the peripheral stretched with TDBE set. Load a
     * dummy byte, then cycle PE so SCL/SDA are released immediately instead of
     * waiting for the open-frame watchdog. */
    I2C2->dt = 0xFFu;
    I2C2->ctrl1_bit.stretch = 0;
    I2C2->ctrl1_bit.i2cen = 0;
    I2C2->ctrl1_bit.i2cen = 1;
    I2C2->ctrl1_bit.acken = 1;

    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
    pending_speed = -1;
    speed_apply_pending = false;
    i2c_select_latch_abort(&selected_latch);
    diag_start_latched = 0u;
    diag_bus_latched = 0u;
}

static void i2c_poll(void)
{
    uint32_t sts1;
    uint32_t relevant;

    sts1 = I2C2->sts1;
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
        I2C2->sts1 = (uint32_t)~(STS1_OUF & 0x0000DF00u);
        i2c_abort_frame();
        return;
    }

    if ((sts1 & STS1_ACKFAIL) != 0u) {
        /* ACKFAIL alone is not proof that the master stopped reading; retain
         * tx_mode until STOPF or a new address match retires this frame.
         * Full 256-byte map reads end with tx_index == 256 and a normal NACK.
         * Short register reads are at most 128 bytes in the tester, so only a
         * later NACK is a useful early-NACK diagnostic. */
        if ((tx_mode != 0u) && (tx_index > 128u) && (tx_index < 255u)) {
            if (diag_early_nack_cnt != 0xFFu) {
                diag_early_nack_cnt++;
            }
            diag_early_nack_pos = (uint8_t)tx_index;
        }
        I2C2->sts1 = (uint32_t)~(STS1_ACKFAIL & 0x0000DF00u);
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

        (void)I2C2->sts1;
        sts2 = I2C2->sts2;
        /* Retire flags left by the previous frame. BUSERR and ARLOST are
         * serviced here because the address branch returns before the tail
         * handler. ACKFAIL was already handled at the top of this call. */
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
                I2C2->sts1 = (uint32_t)~(stale & 0x0000DF00u);
            }
        }
        /* Direction was sampled immediately after the STS1 read that clears
         * ADDRF. Do not move that read below the stale-flag writes above. */
        g_frames++;                 /* one matched address = one transaction */
#ifndef DIAG_BUILD
        last_frame_ms = g_ms;
#endif
        if (fault_drop_frame != 0u) {
            g_frames--;
            fault_drop_frame = 0u;
        }
        /* Direction first: the master starts shifting the first byte within
         * about one SCL bit time of the address match, and the slave loses the
         * MSB if DT is not loaded by then. */
        diag_start_latched = 0u;
        diag_bus_latched = 0u;
        if ((sts2 & STS2_DIRF) != 0u) {
            tx_mode = 1;
            rx_mode = 0;
            tx_index = 0u;
            tx_map = manager_map();     /* one snapshot for the whole frame */
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
            ow_hal_strong_pullup(true);     /* deliberate rail load */
        }
        if (fault_edges != 0u) {
            fault_edges = 0u;
            ow_hal_feed(fault_pulses, 64u); /* 64 slots x 165 us = 10.56 ms */
        }
        return;
    }

    /* No address match pending: retire a STOP.  A STOP is only acted on when
     * the bus really is released, otherwise a spurious flag would cut a live
     * transmit in half.  When the master has already addressed us again, the
     * address branch above retires the old frame unconditionally: the next
     * transaction has usually started by the time this is polled, so requiring
     * an idle bus there would leave tx_mode set forever and starve the
     * manager (measured: 401k transactions, REG_FRAMES never advanced). */
    if ((sts1 & STS1_STOPF) != 0u) {
        if (((GPIOA->idt & (SDA_BIT | SCL_BIT)) == (SDA_BIT | SCL_BIT)) ||
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
        uint8_t b = (uint8_t)I2C2->dt;

        if (rx_bytes != 0xFFFFu) {
            rx_bytes++;
        }
        if ((rx_bytes == 9u) && (diag_misdecode != 0xFFu)) {
            /* A write transaction is at most two bytes; past eight the master
             * is clocking a read while this slave sits in rx_mode.  Counted
             * here, not at the STOP, because such a frame often loses it. */
            diag_misdecode++;
        }

        if (first_byte != 0u) {
            reg_ptr = b;
            first_byte = 0;
        } else {
            /* Only REG_SPEED and REG_CMD are writable.  Everything else is a
             * read-only snapshot; the pointer still advances so a block write
             * cannot be used to poke the register file. */
            if (reg_ptr == REG_SPEED) {
                if (b <= 1u) {
                    g_regs[REG_SPEED] = b;
                    pending_speed = (int)b;
                }
            } else if (reg_ptr == REG_CMD) {
                g_regs[REG_CMD] = b;
                pending_cmd = b;
            } else if (reg_ptr == REG_SELECTED) {
                i2c_select_latch_write(&selected_latch, b);
            } else if (reg_ptr == REG_FAULT) {
                fault_apply(b);
            }
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        }
        return;
    }

    if ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) {
        i2c_send_byte();
        return;
    }

    if ((sts1 & STS1_ACKFAIL) != 0u) {
        /* Handled at the top of this function. */
    }
    if ((sts1 & STS1_BUSERR) != 0u) {
        g_i2c_err_bus++;
        g_err_bus = sat_inc(g_err_bus);
        g_health |= HEALTH_BUSERR;
        I2C2->sts1 = (uint32_t)~(STS1_BUSERR & 0x0000DF00u);
    }
    if ((sts1 & STS1_ARLOST) != 0u) {
        g_i2c_err_arl++;
        g_health |= HEALTH_ARLOST;
        I2C2->sts1 = (uint32_t)~(STS1_ARLOST & 0x0000DF00u);
    }
    if ((sts1 & STS1_OUF) != 0u) {
        /* Handled at the top of this function. */
    }
    /* A broken frame drops the transfer and releases any hardware stretch.
     * A normal read NACK is not an abort; STOP/ADDR retires that frame. */
    if ((sts1 & (STS1_BUSERR | STS1_ARLOST | STS1_OUF)) != 0u) {
        i2c_abort_frame();
    }
}

int main(void)
{
    clock_config();
    timebase_init();
    calibrate_cycles();
    ow_hal_init();
    i2c_hw_init();

    manager_init();

#ifndef DIAG_BUILD
    watchdog_init();
#endif

    tx_map = manager_map();

    for (;;) {
        timebase_poll();
#ifndef DIAG_BUILD
        bus_health_poll();
#endif
        i2c_poll();
        if (!i2c_active()) {
            if (speed_apply_pending &&
                ((int32_t)(DWT->CYCCNT - speed_apply_at_cycles) >= 0) &&
                ((I2C2->sts2 & 0x00000002u) == 0u) &&
                ((GPIOA->idt & (SDA_BIT | SCL_BIT)) == (SDA_BIT | SCL_BIT))) {
                i2c_apply_speed(pending_speed != 0 ? 400000u : 100000u);
                pending_speed = -1;
                speed_apply_pending = false;
            }
            uint8_t st = (uint8_t)(manager_state_reg() & 0x0Fu);
            uint32_t t0 = DWT->CYCCNT;

            manager_poll();
            uint32_t d = DWT->CYCCNT - t0;

            if (d > max_cycles) {
                max_cycles = d;
                max_state = st;         /* state the call started in */
            }
#ifndef DIAG_BUILD
        } else if (i2c_active() &&
                   ((g_ms - last_frame_ms) > BUS_FRAME_TIMEOUT_MS)) {
            /* Master disappeared without STOP. No new address has arrived for
             * longer than any legal transfer on this bus, so retire the stale
             * software frame even if SCL/SDA were left low. */
            i2c_bus_recover();
#endif
        }
#ifndef DIAG_BUILD
        watchdog_kick();
#endif
    }
}

void HardFault_Handler(void)
{
#ifdef DIAG_BUILD
    for (;;) {
    }
#else
    NVIC_SystemReset();
    for (;;) {
    }
#endif
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));













