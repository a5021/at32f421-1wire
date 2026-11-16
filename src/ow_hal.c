#include "ow_hal.h"
#include "at32f421.h"
#include "clock.h"

#define OW_PIN                  10u
#define OW_PIN_MASK             (1u << OW_PIN)
#define OW_PIN_SHIFT            (OW_PIN * 2u)

#define DMA_W_BYTE              0u
#define DMA_W_HALFWORD          1u

static volatile bool ow_active;

static void dma_stop(dma_channel_type *ch)
{
    ch->ctrl = 0u;
}

static void dma_prog(dma_channel_type *ch, volatile void *paddr, uint32_t maddr,
                     uint16_t count, bool m2p, bool minc, bool pinc,
                     uint8_t mwidth, uint8_t pwidth)
{
    ch->ctrl = 0u;
    ch->paddr = (uint32_t)paddr;
    ch->maddr = maddr;
    ch->dtcnt = count;
    ch->ctrl = (1u << 0)                        /* channel enable */
             | ((m2p ? 1u : 0u) << 4)           /* direction */
             | ((pinc ? 1u : 0u) << 6)          /* peripheral increment */
             | ((minc ? 1u : 0u) << 7)          /* memory increment */
             | ((uint32_t)pwidth << 8)          /* peripheral width */
             | ((uint32_t)mwidth << 10)         /* memory width */
             | (2u << 12);                      /* high priority */
}

/* Software update event (UG) followed by a flag flush. */
static inline void ow_ug(void)
{
    TMR1->swevt = 1u;                           /* OVFSWTR */
    (void)TMR1->ists;
    TMR1->ists = 0u;
}

void ow_hal_init(void)
{
    /* Peripheral clocks. */
    CRM->ahben_bit.gpioaen = 1;
    CRM->ahben_bit.dma1en = 1;
    CRM->apb2en_bit.tmr1en = 1;

    /* Timer: 1 us tick, no ARR preload, overflow events enabled (OVFEN=0).
     * TMR1 is on APB2, whose prescaler is /1, so DIV comes straight from the
     * APB2 frequency (120 MHz -> DIV 119). */
    {
        uint32_t mhz = clock_apb2_hz() / 1000000u;

        TMR1->ctrl1 = 0u;
        TMR1->div = (mhz != 0u) ? (mhz - 1u) : 0u;
    }
    TMR1->pr = 0u;
    TMR1->rpr = 0u;
    TMR1->c3dt = 0u;
    TMR1->c4dt = 0u;
    TMR1->c2dt = 0u;
    TMR1->ists = 0u;
    TMR1->iden = 0u;
    TMR1->cctrl = 0u;

    /* CH3 output, PWM mode B (low while CNT < C3DT), output enabled.
     * C3DT = 0 and CNT = 0 keep the line HIGH before the pin is muxed. */
    TMR1->cm2 = (1u << 3) | (7u << 4);
    TMR1->cctrl = (1u << 8);
    TMR1->brk_bit.oen = 1;
    ow_ug();

    /* PA10: idle high, open-drain first, then TMR1_CH3 (MUX 2). */
    GPIOA->odt |= OW_PIN_MASK;
    GPIOA->omode |= OW_PIN_MASK;
    GPIOA->odrvr = (GPIOA->odrvr & ~(3u << OW_PIN_SHIFT)) | (1u << OW_PIN_SHIFT);
    GPIOA->pull = (GPIOA->pull & ~(3u << OW_PIN_SHIFT)) | (1u << OW_PIN_SHIFT);
    GPIOA->muxh = (GPIOA->muxh & ~(0xFu << 8)) | (2u << 8);
    GPIOA->cfgr = (GPIOA->cfgr & ~(3u << OW_PIN_SHIFT)) | (2u << OW_PIN_SHIFT);

    ow_active = false;
}

bool ow_hal_busy(void)
{
    return ow_active;
}

bool ow_hal_done(void)
{
    if (!ow_active) {
        return true;
    }
    if ((TMR1->ists & 1u) == 0u) {             /* OVFIF */
        return false;
    }
    TMR1->ists = 0u;
    /* The pass is over: one-cycle mode stopped the counter at the closing
     * update event, and the CH3 compare value preloaded for that same event
     * left the line released HIGH.  Nothing to stop or release here. */
    ow_active = false;
    return true;
}

/* Start one hardware pass in one-cycle mode: the counter halts at the pass's
 * update event (RPR sets how many periods away that is). */
static void start_one_pulse(void)
{
    TMR1->ctrl1_bit.ocmen = 1u;
    TMR1->ctrl1_bit.tmren = 1u;
}

void ow_hal_feed(const uint8_t *pulses, uint16_t slots)
{
    if ((pulses == 0) || (slots == 0u) || (slots > OW_MAX_SLOTS)) {
        return;
    }

    ow_active = true;

    TMR1->cctrl = 0u;
    TMR1->iden = 0u;
    dma_stop(DMA1_CHANNEL3);

    TMR1->rpr = (uint16_t)(slots - 1u);
    TMR1->pr = OW_T_SLOT;

    if (slots == 1u) {
        /* Single slot: preload mode, release value transferred at the final
         * update event so the bus is released by hardware. */
        TMR1->c3dt = pulses[0];
        TMR1->cm2 = (1u << 3) | (7u << 4);
        ow_ug();
        TMR1->c3dt = 0u;
    } else {
        /* Multi slot: immediate writes, CH2 compare feeds the next C3DT. */
        TMR1->c3dt = pulses[0];
        TMR1->c2dt = OW_T_LOW1 + OW_T_LOW0;
        TMR1->cm2 = (7u << 4);
        TMR1->iden = (1u << 10);                /* C2DEN */
        ow_ug();

        dma_prog(DMA1_CHANNEL3, &TMR1->c3dt, (uint32_t)&pulses[1], slots,
                 true, true, false, DMA_W_BYTE, DMA_W_HALFWORD);
    }

    TMR1->cctrl = (1u << 8);                    /* C3EN */
    start_one_pulse();
}

static void capture_arm(volatile void *dst, uint16_t count, uint16_t first,
                        uint16_t period, uint16_t rpr, uint8_t mwidth)
{
    ow_active = true;

    TMR1->cctrl = 0u;
    TMR1->iden = 0u;
    dma_stop(DMA1_CHANNEL4);

    TMR1->rpr = rpr;
    TMR1->pr = period;
    TMR1->c3dt = first;
    TMR1->cm2 = (1u << 3) | (7u << 4)           /* CH3 preload, PWM B */
              | (2u << 8)                        /* C4C = C3IFP4 */
              | ((uint32_t)OW_C4DF << 12);       /* CH4 filter */
    ow_ug();

    /* Release value for the closing update event: CH3 is preloaded, so the
     * zero waits in the shadow register and only reaches the compare when the
     * pass ends.  At that same event one-cycle mode stops the counter with the
     * output already HIGH, i.e. the line released by hardware. */
    TMR1->c3dt = 0u;

    dma_prog(DMA1_CHANNEL4, &TMR1->c4dt, (uint32_t)dst, count,
             false, true, false, mwidth, DMA_W_HALFWORD);

    TMR1->iden = (1u << 12);                     /* C4DEN */
    TMR1->cctrl = (1u << 8) | (1u << 12);        /* C3EN | C4EN */
    start_one_pulse();
}

void ow_hal_reset(void)
{
    static volatile uint16_t cap[2];
    capture_arm(cap, 2u, OW_T_RESET_LOW, OW_T_RESET_PERIOD, 0u, DMA_W_HALFWORD);
}

void ow_hal_read(volatile uint8_t *dst, uint16_t slots)
{
    if ((dst == 0) || (slots == 0u) || (slots > OW_MAX_SLOTS)) {
        return;
    }
    capture_arm(dst, slots, OW_T_LOW1, OW_T_SLOT, (uint16_t)(slots - 1u), DMA_W_BYTE);
}

void ow_hal_read_pair(volatile uint8_t *dst2)
{
    if (dst2 == 0) {
        return;
    }
    capture_arm(dst2, 2u, OW_T_LOW1, OW_T_SLOT, 1u, DMA_W_BYTE);
}

void ow_hal_wait(uint32_t us)
{
    uint32_t n = (us + 65534u) / 65535u;
    uint32_t period;

    if (n == 0u) {
        n = 1u;
    }
    if (n > 256u) {
        n = 256u;
    }
    period = (us + n - 1u) / n;
    if (period == 0u) {
        period = 1u;
    }
    if (period > 65535u) {
        period = 65535u;
    }

    ow_active = true;

    TMR1->cctrl = 0u;
    TMR1->iden = 0u;
    dma_stop(DMA1_CHANNEL3);
    dma_stop(DMA1_CHANNEL4);

    TMR1->rpr = (uint16_t)(n - 1u);
    TMR1->pr = (uint16_t)period;
    TMR1->c3dt = 0u;
    TMR1->cm2 = (1u << 3) | (7u << 4);
    ow_ug();

    TMR1->cctrl = (1u << 8);
    start_one_pulse();
}

uint16_t ow_hal_captured(uint16_t programmed)
{
    uint16_t left = (uint16_t)DMA1_CHANNEL4->dtcnt_bit.cnt;
    return (left <= programmed) ? (uint16_t)(programmed - left) : 0u;
}

void ow_hal_strong_pullup(bool on)
{
    if (on) {
        /* Force the timer output HIGH (CNT = 0, C3DT = 0, PWM mode B). */
        GPIOA->odt |= OW_PIN_MASK;
        TMR1->c3dt = 0u;
        TMR1->swevt = 1u;
        (void)TMR1->ists;
        TMR1->ists = 0u;
        TMR1->cctrl |= (1u << 8);
        GPIOA->omode &= ~OW_PIN_MASK;           /* push-pull */
    } else {
        GPIOA->omode |= OW_PIN_MASK;            /* open-drain */
        TMR1->cctrl &= ~(1u << 8);              /* C3EN off */
        TMR1->ctrl1_bit.tmren = 0u;             /* stop the timer */
    }
}
