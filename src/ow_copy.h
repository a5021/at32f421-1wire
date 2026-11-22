#ifndef OW_COPY_H
#define OW_COPY_H

#include <stdint.h>

/* Fixed-size copies for aligned buffers.  A single word load/store pair beats
 * a memcpy call here: these copies sit inside the manager steps that must stay
 * short for the polled I2C slave. */
typedef uint32_t __attribute__((may_alias)) ow_word_t;

/* Both pointers must be 4-byte aligned. */
static inline void ow_copy8(void *dst, const void *src)
{
    ow_word_t *d = (ow_word_t *)dst;
    const ow_word_t *s = (const ow_word_t *)src;

    d[0] = s[0];
    d[1] = s[1];
}

/* Both pointers must be 4-byte aligned. */
static inline void ow_copy9(uint8_t *dst, const uint8_t *src)
{
    ow_word_t *d = (ow_word_t *)dst;
    const ow_word_t *s = (const ow_word_t *)src;

    d[0] = s[0];
    d[1] = s[1];
    dst[8] = src[8];
}

/* Both pointers must be 4-byte aligned. */
static inline void ow_fill8_ff(void *dst)
{
    ow_word_t *d = (ow_word_t *)dst;

    d[0] = 0xFFFFFFFFu;
    d[1] = 0xFFFFFFFFu;
}

#endif
