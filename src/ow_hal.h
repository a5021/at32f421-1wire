#ifndef OW_HAL_H
#define OW_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include "ow_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register-level 1-Wire engine for AT32F421: TMR1 + DMA1.
 *
 * PA10 = TMR1_CH3, MUX 2.  TMR1 runs at 1 MHz (120 MHz / 120).
 * Feed:   DMA1 channel 3 (TMR1_CH2 compare) writes C3DT.
 * Capture: DMA1 channel 4 (TMR1_CH4) stores the counter at rising edges.
 *
 * All start functions are non-blocking.  Completion is observed through
 * ow_hal_done().  No interrupts are used.
 */

void     ow_hal_init(void);

/* Non-zero while a hardware pass is running (software flag). */
bool     ow_hal_busy(void);

/* Test-and-clear the completion flag.  Returns true when no pass is pending
 * or the active pass just finished. */
bool     ow_hal_done(void);

/* Transmit `slots` bit slots. pulses[0..slots-1] hold the low-phase duration
 * in microseconds (5 or 60), pulses[slots] must be 0 (bus release). */
void     ow_hal_feed(const uint8_t *pulses, uint16_t slots);

/* Reset pulse + presence capture (two rising edges, 16-bit). */
void     ow_hal_reset(void);

/* Read `slots` bit slots into dst[] (8-bit counter values at rising edges). */
void     ow_hal_read(volatile uint8_t *dst, uint16_t slots);

/* Read two slots (search ROM id/complement pair). */
void     ow_hal_read_pair(volatile uint8_t *dst2);

/* Hardware-timed delay; completion through ow_hal_done(). */
void     ow_hal_wait(uint32_t us);

/* Number of captures actually stored for a pass programmed with `programmed`. */
uint16_t ow_hal_captured(uint16_t programmed);

/* PA10 open-drain <-> push-pull.  The timer output is forced HIGH first. */
void     ow_hal_strong_pullup(bool on);

#ifdef __cplusplus
}
#endif

#endif /* OW_HAL_H */
