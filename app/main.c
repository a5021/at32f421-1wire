#include <stdint.h>
#include <string.h>

#include "at32f421.h"
#include "clock.h"
#include "manager.h"
#include "ow_hal.h"
#include "onewire.h"
#include "ds18b20.h"
#include "ds2431.h"

#define I2C_ADDR7               0x42u
#define I2C_OADDR1              ((uint16_t)(I2C_ADDR7 << 1))

#define STS1_ADDR7F             0x00000002u
#define STS1_STOPF              0x00000010u
#define STS1_RDBF               0x00000040u
#define STS1_TDBE               0x00000080u
#define STS1_BUSERR             0x00000100u
#define STS1_ARLOST             0x00000200u
#define STS1_ACKFAIL            0x00000400u
#define STS1_OUF                0x00000800u
#define STS2_DIRF               0x00000004u

extern volatile uint32_t g_i2c_speed;

extern uint8_t g_regs[256];

static uint32_t reg_ptr;
static uint32_t rx_mode;
static uint32_t tx_mode;
static uint32_t first_byte;
static int      pending_speed = -1;
static uint8_t  pending_cmd;

volatile uint32_t g_i2c_err_ack;
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
static uint32_t g_ms;               /* 1 ms tick, polled from SysTick */
static uint32_t g_scl_hz = 100000u; /* timing currently programmed */
static uint32_t sda_low_since;
static uint32_t last_recovery_ms;
static bool     sda_low_seen;

#define SDA_BIT                 0x00000002u
#define SCL_BIT                 0x00000001u
#define STUCK_MS                10u
#define RECOVERY_RETRY_MS       100u

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
    SysTick->CTRL = 0x00000005u;    /* HCLK/8, enable, no tick interrupt */
}

static void timebase_poll(void)
{
    if ((SysTick->CTRL & 0x00010000u) != 0u) {
        g_ms++;
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
    default:
        return g_regs[addr & 0xFFu];
    }
}

static void health_clear(void)
{
    g_frames = 0u;
    g_health = 0u;
    g_err_bus = 0u;
    g_i2c_err_ack = 0u;
    g_i2c_err_bus = 0u;
    g_i2c_err_arl = 0u;
    g_i2c_err_ouf = 0u;
}

bool manager_i2c_busy(void)
{
    return (I2C2->sts2 & 0x00000002u) != 0u;
}

static bool i2c_active(void)
{
    return (tx_mode != 0u) || (rx_mode != 0u) || manager_i2c_busy();
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
        rise = 0x3Fu;
    }

    I2C2->clkctrl_bit.dutymode = 0;             /* 1:2 high:low in fast mode */
    I2C2->clkctrl_bit.speed = speed;
    I2C2->tmrise_bit.risetime = rise;

    I2C2->ctrl1_bit.i2cen = 1;
    I2C2->ctrl1_bit.acken = 1;
    I2C2->ctrl1_bit.stretch = 1;

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

/* Nine clocks and a STOP free a slave that was left mid-byte.  Called only
 * after SDA has been idle-low for STUCK_MS with no matched address, so the
 * master cannot be talking: driving the pins here is safe. */
static void i2c_bus_recover(void)
{
    I2C2->ctrl1_bit.i2cen = 0;

    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x00000066u;  /* OD output */
    GPIOA->omode |= 0x00000003u;
    spin_us(5u);

    for (int i = 0; i < 9; i++) {
        GPIOA->omode &= ~SCL_BIT;
        spin_us(5u);
        GPIOA->omode |= SCL_BIT;
        spin_us(5u);
    }

    GPIOA->omode &= ~SDA_BIT;       /* STOP: SDA low, SCL high, SDA high */
    spin_us(5u);
    GPIOA->omode |= SCL_BIT;
    spin_us(5u);
    GPIOA->omode |= SDA_BIT;
    spin_us(5u);

    GPIOA->cfgr = (GPIOA->cfgr & ~0x0000000Fu) | 0x0000000Au;
    GPIOA->omode |= 0x00000003u;

    i2c_hw_init();

    reg_ptr = 0u;
    rx_mode = 0u;
    tx_mode = 0u;
    first_byte = 0u;
    pending_cmd = 0u;
    pending_speed = -1;
    sda_low_seen = false;
    last_recovery_ms = g_ms;

    if ((GPIOA->idt & SDA_BIT) != 0u) {
        g_health |= HEALTH_RECOVERED;
    }
}

/* SDA held low outside any transaction means somebody, most likely this slave
 * after an aborted byte, still owns the bus.  SCL is not checked: a low SCL is
 * this slave's own clock stretch and perfectly legal.  STS2.busy is useless
 * here because a master pulling SDA low is decoded as a START, so the only
 * reliable evidence is SDA low for STUCK_MS with no matched address. */
static void bus_health_poll(void)
{
    if ((GPIOA->idt & SDA_BIT) != 0u) {
        sda_low_seen = false;
        return;
    }

    if (!sda_low_seen) {
        sda_low_seen = true;
        sda_low_since = g_ms;
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

static void i2c_poll(void)
{
    uint32_t sts1 = I2C2->sts1;
    uint32_t relevant = sts1 & ~STS1_TDBE;

    if ((relevant == 0u) && (tx_mode == 0u)) {
        return;
    }

    if ((sts1 & STS1_ADDR7F) != 0u) {
        uint32_t sts2 = I2C2->sts2;

        (void)I2C2->sts1;
        (void)I2C2->sts2;

        g_frames++;                 /* one matched address = one transaction */
        sda_low_seen = false;

        if ((sts2 & STS2_DIRF) != 0u) {
            tx_mode = 1;
            rx_mode = 0;
            I2C2->dt = reg_byte(reg_ptr);
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        } else {
            rx_mode = 1;
            tx_mode = 0;
            first_byte = 1;
        }
        return;
    }

    if ((rx_mode != 0u) && ((sts1 & STS1_RDBF) != 0u)) {
        uint8_t b = (uint8_t)I2C2->dt;

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
                g_regs[REG_SELECTED] = b & 0x0Fu;
            }
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        }
        return;
    }

    if ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) {
        I2C2->dt = reg_byte(reg_ptr);
        reg_ptr = (reg_ptr + 1u) & 0xFFu;
        return;
    }

    if ((sts1 & STS1_STOPF) != 0u) {
        (void)I2C2->sts1;
        I2C2->ctrl1_bit.i2cen = 1;
        rx_mode = 0;
        tx_mode = 0;
        first_byte = 0;

        if (pending_cmd != 0u) {
            if (pending_cmd == 3u) {
                health_clear();
            }
            manager_command(pending_cmd);
            pending_cmd = 0;
        }
        if (pending_speed >= 0) {
            i2c_apply_speed(pending_speed != 0 ? 400000u : 100000u);
            pending_speed = -1;
        }
        return;
    }

    if ((sts1 & STS1_ACKFAIL) != 0u) {
        g_i2c_err_ack++;
        I2C2->sts1 = (uint32_t)~(STS1_ACKFAIL & 0x0000DF00u);
        tx_mode = 0;
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
        g_i2c_err_ouf++;
        g_health |= HEALTH_OUF;
        I2C2->sts1 = (uint32_t)~(STS1_OUF & 0x0000DF00u);
    }
    /* An aborted transfer must not leave a command or a speed change armed
     * for the next STOP, which belongs to somebody else's transaction. */
    if ((sts1 & (STS1_ACKFAIL | STS1_BUSERR | STS1_ARLOST | STS1_OUF)) != 0u) {
        pending_cmd = 0;
        pending_speed = -1;
        rx_mode = 0;
        tx_mode = 0;
        first_byte = 0;
    }
}

int main(void)
{
    clock_config();
    timebase_init();
    ow_hal_init();
    i2c_hw_init();

    manager_init();

    for (;;) {
        timebase_poll();
        bus_health_poll();
        i2c_poll();
        if (!i2c_active()) {
            manager_poll();
        }
    }
}

void HardFault_Handler(void)
{
    for (;;) {
    }
}

void MemManage_Handler(void) __attribute__((alias("HardFault_Handler")));
void BusFault_Handler(void) __attribute__((alias("HardFault_Handler")));
void UsageFault_Handler(void) __attribute__((alias("HardFault_Handler")));
