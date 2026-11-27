#ifndef OW_CONFIG_H
#define OW_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * 1-Wire bus timing, microseconds. The timer runs at 1 MHz (1 us per tick).
 *
 *   low1     write-1 low phase / read-slot init low phase
 *   low0     write-0 low phase
 *   guard    recovery time after every slot. The default 100 us makes a
 *            165 us slot, intentionally above the DS18B20 120 us maximum to
 *            lower edge density on this wired multi-device harness. Builds
 *            that require strict timing can use -DOW_T_GUARD=55 (120 us slot).
 */
#ifndef OW_T_LOW1
#define OW_T_LOW1               5u
#endif
#ifndef OW_T_LOW0
#define OW_T_LOW0               60u
#endif
#ifndef OW_T_GUARD
#define OW_T_GUARD              100u
#endif

#define OW_T_SLOT               (OW_T_LOW1 + OW_T_LOW0 + OW_T_GUARD)

#ifndef OW_T_RESET_LOW
#define OW_T_RESET_LOW          480u
#endif
#ifndef OW_T_RESET_PERIOD
#define OW_T_RESET_PERIOD       960u
#endif

/* Captured counter value at or below this is a logic 1 (read slot). */
#ifndef OW_T_SAMPLE_MAX
#define OW_T_SAMPLE_MAX         10u
#endif

/* CH4 digital filter (C4DF), start small and tune on a logic analyzer. */
#ifndef OW_C4DF
#define OW_C4DF                 0x3u
#endif

/* TMR1 RPR is 8 bits: at most 256 slots per hardware pass. */
#define OW_MAX_SLOTS            256u
#define OW_MAX_BYTES            (OW_MAX_SLOTS / 8u)

#endif /* OW_CONFIG_H */
