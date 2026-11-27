#include <stdint.h>

#include "at32f421.h"
#include "clock.h"
#include "i2c_hw.h"

#define I2C_ADDR7               0x42u
#define I2C_OADDR1              ((uint16_t)(I2C_ADDR7 << 1))
#define SDA_BIT                 0x00000002u
#define SCL_BIT                 0x00000001u

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
}

void i2c_hw_init(uint32_t scl_hz)
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

    i2c_apply_speed(scl_hz);
}

uint32_t i2c_hw_sts1(void)
{
    return I2C2->sts1;
}

uint32_t i2c_hw_sts2(void)
{
    return I2C2->sts2;
}

uint32_t i2c_hw_gpio_idt(void)
{
    return GPIOA->idt;
}

uint8_t i2c_hw_dt_read(void)
{
    return (uint8_t)I2C2->dt;
}

void i2c_hw_dt_write(uint8_t value)
{
    I2C2->dt = value;
}

void i2c_hw_clear_flags(uint32_t flags)
{
    I2C2->sts1 = (uint32_t)~(flags & 0x0000DF00u);
}

void i2c_hw_stop_rearm(void)
{
    I2C2->ctrl1_bit.i2cen = 1;
}

void i2c_hw_abort(void)
{
    /* A failed frame can leave the peripheral stretched with TDBE set. Load a
     * dummy byte, then cycle PE so SCL/SDA are released immediately instead of
     * waiting for the open-frame watchdog. */
    I2C2->dt = 0xFFu;
    I2C2->ctrl1_bit.i2cen = 0;
    I2C2->ctrl1_bit.stretch = 0;
    I2C2->ctrl1_bit.i2cen = 1;
    I2C2->ctrl1_bit.acken = 1;
}

void i2c_hw_apply_speed(uint32_t scl_hz)
{
    i2c_apply_speed(scl_hz);
}

bool i2c_hw_busy(void)
{
    return (I2C2->sts2 & 0x00000002u) != 0u;
}

static void spin_us(uint32_t us)
{
    while (us-- != 0u) {
        for (volatile uint32_t i = 0u; i < 60u; i++) {
        }
    }
}

/* Nine clocks and a STOP free a slave left mid-byte. The caller must prove
 * the master stopped talking: either no address arrived for the open-frame
 * timeout, or SDA has been stuck low with SCL high for STUCK_MS. */
i2c_recovery_result_t i2c_hw_recover(uint32_t scl_hz, bool allow_low_scl)
{
    bool scl_ok = true;
    bool recovered;

    if (!allow_low_scl && ((GPIOA->idt & SCL_BIT) == 0u)) {
        return I2C_RECOVERY_DEFERRED;
    }

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
    i2c_hw_init(scl_hz);

    recovered = scl_ok &&
                ((GPIOA->idt & (SDA_BIT | SCL_BIT)) == (SDA_BIT | SCL_BIT));
    return recovered ? I2C_RECOVERY_OK : I2C_RECOVERY_FAILED;
}

void i2c_hw_delay_us(uint32_t us)
{
    spin_us(us);
}

uint32_t i2c_hw_cycles(void)
{
    return DWT->CYCCNT;
}
