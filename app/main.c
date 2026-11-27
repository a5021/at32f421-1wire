#include <stdint.h>

#include "at32f421.h"
#include "clock.h"
#include "i2c_hw.h"
#include "i2c_slave.h"
#include "manager.h"
#include "ow_hal.h"

#define WDT_CMD_UNLOCK         0x00005555u
#define WDT_CMD_ENABLE         0x0000CCCCu
#define WDT_CMD_RELOAD         0x0000AAAAu
#define WDT_DIV_64             0x00000004u
#define WDT_RELOAD_4S          2500u
#define WDT_CMD                (*(volatile uint32_t *)(WDT_BASE + 0x00u))
#define WDT_DIV                (*(volatile uint32_t *)(WDT_BASE + 0x04u))
#define WDT_RLD                (*(volatile uint32_t *)(WDT_BASE + 0x08u))
#define WDT_STS                (*(volatile uint32_t *)(WDT_BASE + 0x0Cu))

static uint32_t g_ms;

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

    uint32_t cycles = DWT->CYCCNT - t0;

    if ((cycles > 5000u) && (cycles < 12000000u)) {
        i2c_slave_set_cycles_per_us(cycles / 50000u);
    }
}

bool manager_i2c_busy(void)
{
    return i2c_slave_busy();
}

int main(void)
{
    clock_config();
    timebase_init();
    calibrate_cycles();
    ow_hal_init();
    i2c_hw_init(100000u);

    manager_init();
    i2c_slave_init();

#ifndef DIAG_BUILD
    watchdog_init();
#endif

    for (;;) {
        timebase_poll();
#ifndef DIAG_BUILD
        i2c_slave_health_poll(g_ms);
#endif
        i2c_slave_poll();
        if (!i2c_slave_busy()) {
            uint8_t state = (uint8_t)(manager_state_reg() & 0x0Fu);
            uint32_t t0 = DWT->CYCCNT;

            manager_poll();
            uint32_t duration = DWT->CYCCNT - t0;

            i2c_slave_note_manager_poll(duration, state);
#ifndef DIAG_BUILD
        } else if (i2c_slave_recovery_due(g_ms)) {
            /* Master disappeared without STOP. Retire the stale software
             * frame even if SCL/SDA were left low. */
            i2c_slave_recover(g_ms);
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
