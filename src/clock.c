#include "clock.h"
#include "at32f421.h"

#include <stdbool.h>

#define PLL_MULT_CODE(mult)     ((uint32_t)((mult) - 2u))

void clock_config(void)
{
    uint32_t mult;
    uint32_t fref;
    bool hext_ok;

    /* 120 MHz needs 3 wait states. */
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
        mult = PLL_MULT_CODE(15u);              /* 8 MHz x 15 = 120 MHz */
    } else {
        CRM->ctrl_bit.hexten = 0;
        CRM->cfg_bit.pllrcs = 0;                /* HICK / 2 = 4 MHz */
        fref = 0u;                              /* 3.9..5 MHz */
        mult = PLL_MULT_CODE(30u);              /* 4 MHz x 30 = 120 MHz */
    }

    CRM->cfg_bit.pllhextdiv = 0;                /* no HEXT pre-divider */
    CRM->cfg_bit.pllmult_l = mult & 0x0Fu;
    CRM->cfg_bit.pllmult_h = (mult >> 4) & 0x03u;
    CRM->pll_bit.pllfref = fref;
    CRM->pll_bit.pllcfgen = 0u;
    CRM->ctrl_bit.pllen = 1;
    while (CRM->ctrl_bit.pllstbl == 0u) {
    }

    /* AHB, APB1 and APB2 all run at 120 MHz. */
    CRM->cfg_bit.ahbdiv = 0u;
    CRM->cfg_bit.apb1div = 0u;
    CRM->cfg_bit.apb2div = 0u;

    CRM->cfg_bit.sclksel = 2u;                  /* PLL */
    while (CRM->cfg_bit.sclksts != 2u) {
    }

    /* The BSP helper crm_sysclk_switch_status_get() is not linked here. */
    system_core_clock = 120000000u;
}
