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

bool manager_i2c_busy(void)
{
    return (I2C2->sts2 & 0x00000002u) != 0u;
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

    i2c_apply_speed(100000u);
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

        if ((sts2 & STS2_DIRF) != 0u) {
            tx_mode = 1;
            rx_mode = 0;
            I2C2->dt = g_regs[reg_ptr & 0xFFu];
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
            g_regs[reg_ptr] = b;
            if (reg_ptr == REG_SPEED && b <= 1u) {
                pending_speed = (int)b;
            } else if (reg_ptr == REG_CMD) {
                pending_cmd = b;
            }
            reg_ptr = (reg_ptr + 1u) & 0xFFu;
        }
        return;
    }

    if ((tx_mode != 0u) && ((sts1 & STS1_TDBE) != 0u)) {
        I2C2->dt = g_regs[reg_ptr & 0xFFu];
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
        I2C2->sts1 = (uint32_t)~(STS1_ACKFAIL & 0x0000DF00u);
        tx_mode = 0;
    }
    if ((sts1 & STS1_BUSERR) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_BUSERR & 0x0000DF00u);
    }
    if ((sts1 & STS1_ARLOST) != 0u) {
        I2C2->sts1 = (uint32_t)~(STS1_ARLOST & 0x0000DF00u);
    }
    if ((sts1 & STS1_OUF) != 0u) {
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
    ow_hal_init();
    i2c_hw_init();

    manager_init();

    for (;;) {
        i2c_poll();
        manager_poll();
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
