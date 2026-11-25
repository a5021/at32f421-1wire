#include "clock.h"
#include "at32f421.h"

#include <stdbool.h>

#define HEXT_HZ                 8000000u
#define HICK_HZ                 8000000u

/*
 * PLLMULT field encoding (see CRM CFG in the reference manual): the field
 * holds multiplier - 2 for x2..x15 and multiplier - 1 from x16 upwards, with
 * entry 15 belonging to the upper range.
 */
static uint32_t pll_field(uint32_t mult)
{
    return (mult >= 16u) ? (mult - 1u) : (mult - 2u);
}

static uint32_t pll_multiplier(void)
{
    uint32_t code = (uint32_t)CRM->cfg_bit.pllmult_l
                  | ((uint32_t)CRM->cfg_bit.pllmult_h << 4u);

    return (code >= 15u) ? (code + 1u) : (code + 2u);
}

static uint32_t ahb_shift(uint32_t div)
{
    static const uint8_t table[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 6, 7, 8, 9};

    return table[div & 0x0Fu];
}

static uint32_t apb_shift(uint32_t div)
{
    static const uint8_t table[8] = {0, 0, 0, 0, 1, 2, 3, 4};

    return table[div & 0x07u];
}

static uint32_t sclk_hz(void)
{
    switch (CRM->cfg_bit.sclksts) {
    case 1u:                                    /* HEXT */
        return HEXT_HZ;
    case 2u: {                                  /* PLL */
        uint32_t src;

        if (CRM->cfg_bit.pllrcs == 0u) {
            src = HICK_HZ / 2u;                 /* HICK / 2 */
        } else if (CRM->cfg_bit.pllhextdiv != 0u) {
            src = HEXT_HZ / 2u;
        } else {
            src = HEXT_HZ;
        }
        return src * pll_multiplier();
    }
    default:                                    /* HICK */
        return HICK_HZ;
    }
}

uint32_t clock_ahb_hz(void)
{
    return sclk_hz() >> ahb_shift(CRM->cfg_bit.ahbdiv);
}

uint32_t clock_apb1_hz(void)
{
    return clock_ahb_hz() >> apb_shift(CRM->cfg_bit.apb1div);
}

uint32_t clock_apb2_hz(void)
{
    return clock_ahb_hz() >> apb_shift(CRM->cfg_bit.apb2div);
}

void clock_config(void)
{
    uint32_t mult;
    uint32_t fref;
    bool hext_ok;

    /* 120 MHz needs 3 wait states (fHCLK > 96 MHz). */
    FLASH->psr = 0x150u | 3u;

    /* Prefer HEXT when a crystal is fitted; otherwise run the PLL from
     * HICK/2.  Both paths produce exactly 120 MHz, so the 1 us timer tick
     * and all 1-Wire timings stay valid. */
    CRM->ctrl_bit.hexten = 1;
    for (volatile uint32_t i = 0u; i < 400000u; i++) {
        if (CRM->ctrl_bit.hextstbl != 0u) {
            break;
        }
    }
    hext_ok = (CRM->ctrl_bit.hextstbl != 0u);

    if (hext_ok) {
        CRM->cfg_bit.pllrcs = 1;                /* HEXT */
        fref = 2u;                              /* 7.8125..8.33 MHz */
        mult = 15u;                             /* 8 MHz x 15 = 120 MHz */
    } else {
        CRM->ctrl_bit.hexten = 0;
        CRM->cfg_bit.pllrcs = 0;                /* HICK / 2 = 4 MHz */
        fref = 0u;                              /* 3.9..5 MHz */
        mult = 30u;                             /* 4 MHz x 30 = 120 MHz */
    }

    CRM->cfg_bit.pllhextdiv = 0;                /* no HEXT pre-divider */
    CRM->cfg_bit.pllmult_l = pll_field(mult) & 0x0Fu;
    CRM->cfg_bit.pllmult_h = (pll_field(mult) >> 4) & 0x03u;
    CRM->pll_bit.pllfref = fref;
    CRM->pll_bit.pllcfgen = 0u;
    CRM->ctrl_bit.pllen = 1;
    {
        uint32_t guard = 4000000u;

        while ((CRM->ctrl_bit.pllstbl == 0u) && (guard != 0u)) {
            guard--;
        }
        if (CRM->ctrl_bit.pllstbl == 0u) {
            /* No PLL, no hang: drop back to HICK, the reset clock, and let
             * clock_ahb_hz() publish the frequency that is actually there. */
            CRM->ctrl_bit.pllen = 0u;
            CRM->cfg_bit.sclksel = 0u;          /* HICK */
            system_core_clock = clock_ahb_hz();
            return;
        }
    }

    /* AHB, APB1 and APB2 all run at 120 MHz. */
    CRM->cfg_bit.ahbdiv = 0u;
    CRM->cfg_bit.apb1div = 0u;
    CRM->cfg_bit.apb2div = 0u;

    CRM->cfg_bit.sclksel = 2u;                  /* PLL */
    {
        uint32_t guard = 100000u;

        while ((CRM->cfg_bit.sclksts != 2u) && (guard != 0u)) {
            guard--;
        }
        if (CRM->cfg_bit.sclksts != 2u) {
            CRM->ctrl_bit.pllen = 0u;
            CRM->cfg_bit.sclksel = 0u;          /* HICK */
        }
    }

    /* Publish what the tree really runs at: the PLL field is decoded back
     * from the register, so a wrong multiplier cannot silently leave the
     * timer tick and the I2C dividers scaled for a clock that is not there. */
    system_core_clock = clock_ahb_hz();
}
