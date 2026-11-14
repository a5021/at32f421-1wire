#ifndef CLOCK_H
#define CLOCK_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SCLK 120 MHz from HEXT 8 MHz (PLL x15) when a crystal is present, or from
 * HICK/2 4 MHz (PLL x30) on a crystal-less board.  AHB/APB1/APB2 = /1,
 * 3 flash wait states.  Call once from main() before peripheral init.
 */
void clock_config(void);

#ifdef __cplusplus
}
#endif

#endif /* CLOCK_H */
