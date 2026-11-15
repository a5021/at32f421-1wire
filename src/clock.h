#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SCLK 120 MHz from HEXT 8 MHz (PLL x15) when a crystal is present, or from
 * HICK/2 4 MHz (PLL x30) on a crystal-less board.  AHB/APB1/APB2 = /1,
 * 3 flash wait states (fHCLK > 96 MHz).  Call once from main() before
 * peripheral init.
 */
void clock_config(void);

/* Frequencies decoded back from the CRM registers, so every divider is taken
 * from the tree that is actually running, not from a build-time constant. */
uint32_t clock_ahb_hz(void);
uint32_t clock_apb1_hz(void);
uint32_t clock_apb2_hz(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_H */
