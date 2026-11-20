#include "scl_probe.h"

#include <string.h>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"
#include "scl_probe.h"

/* Hand-assembled `in pins, 1` plus autopush.  Writing the encoding here keeps
 * the build independent of the pioasm host tool. */
static const uint16_t scl_sampler_instructions[] = { 0x4000u };
static const pio_program_t scl_sampler_program = {
    .instructions = scl_sampler_instructions,
    .length = 1,
    .origin = -1,
};

#define SAMPLER_WORDS       16384u
#define SAMPLER_CLKDIV      2.0f
#define HIST_BIN_NS         16u
#define HIST_BINS           4096u

static PIO      sampler_pio = pio0;
static uint     sampler_sm;
static uint     sampler_offset;
static uint     dma_chan;
static uint32_t samples[SAMPLER_WORDS];
static float    sample_ns = 16.0f;
static uint32_t hist[HIST_BINS];

void scl_probe_init(unsigned pin)
{
    sampler_offset = pio_add_program(sampler_pio, &scl_sampler_program);
    sampler_sm = (uint)pio_claim_unused_sm(sampler_pio, true);

    pio_sm_config c = pio_get_default_sm_config();

    sm_config_set_wrap(&c, sampler_offset, sampler_offset);
    sm_config_set_in_pins(&c, pin);
    sm_config_set_in_shift(&c, true, true, 32u);
    sm_config_set_clkdiv(&c, SAMPLER_CLKDIV);
    pio_sm_init(sampler_pio, sampler_sm, sampler_offset, &c);

    dma_chan = (uint)dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);

    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(sampler_pio, sampler_sm, false));
    dma_channel_configure(dma_chan, &dc, samples, &sampler_pio->rxf[sampler_sm],
                          SAMPLER_WORDS, false);

    uint32_t sys_hz = clock_get_hz(clk_sys);
    sample_ns = 1e9f / ((float)sys_hz / SAMPLER_CLKDIV);
}

void scl_probe_start(void)
{
    pio_sm_set_enabled(sampler_pio, sampler_sm, false);
    pio_sm_clear_fifos(sampler_pio, sampler_sm);
    dma_channel_abort(dma_chan);

    dma_channel_set_write_addr(dma_chan, samples, false);
    dma_channel_set_trans_count(dma_chan, SAMPLER_WORDS, false);
    dma_channel_set_read_addr(dma_chan, &sampler_pio->rxf[sampler_sm], false);

    pio_sm_exec(sampler_pio, sampler_sm,
                pio_encode_jmp((int)sampler_offset));
    pio_sm_set_enabled(sampler_pio, sampler_sm, true);
    dma_channel_start(dma_chan);
}

uint32_t scl_probe_stop(void)
{
    uint32_t remaining = dma_channel_hw_addr(dma_chan)->transfer_count;

    pio_sm_set_enabled(sampler_pio, sampler_sm, false);
    dma_channel_abort(dma_chan);

    if (remaining > SAMPLER_WORDS) {
        remaining = SAMPLER_WORDS;
    }
    return SAMPLER_WORDS - remaining;
}

bool scl_probe_analyze(uint32_t words, scl_stats_t *st)
{
    uint32_t prev_rise = 0;
    uint32_t last_rise = 0;
    uint32_t min_ns = UINT32_MAX;
    uint32_t max_ns = 0;
    uint32_t max_low_ns = 0;
    uint32_t min_low_ns = UINT32_MAX;
    uint32_t count = 0;
    uint32_t counted = 0;
    uint32_t idx = 0;
    bool have_rise = false;
    int prev = -1;

    memset(st, 0, sizeof(*st));
    memset(hist, 0, sizeof(hist));

    if (words > SAMPLER_WORDS) {
        words = SAMPLER_WORDS;
    }

    for (uint32_t w = 0u; w < words; w++) {
        uint32_t word = samples[w];

        for (uint32_t bit = 0u; bit < 32u; bit++) {
            int level = (int)((word >> bit) & 1u);

            if (prev >= 0 && level != prev) {
                if (level == 1) {
                    /* rising edge */
                    if (have_rise) {
                        uint32_t period = (uint32_t)((float)(idx - prev_rise) *
                                                     sample_ns);
                        uint32_t low = (uint32_t)((float)(idx - last_rise) *
                                                  sample_ns);

                        if (period < min_ns) {
                            min_ns = period;
                        }
                        if (period > max_ns) {
                            max_ns = period;
                        }
                        if (low > max_low_ns) {
                            max_low_ns = low;
                        }
                        if (low < min_low_ns) {
                            min_low_ns = low;
                        }
                        if (period != 0u) {
                            uint32_t bin = period / HIST_BIN_NS;

                            if (bin < HIST_BINS) {
                                hist[bin]++;
                                counted++;
                            }
                        }
                        count++;
                    }
                    prev_rise = idx;
                    have_rise = true;
                } else {
                    /* falling edge: remember when the high phase started */
                    last_rise = idx;
                }
                prev = level;
            } else if (prev < 0) {
                prev = level;
            }
            idx++;
        }
    }

    if (count == 0u || min_ns == UINT32_MAX) {
        return false;
    }

    uint32_t target = counted / 2u;
    uint32_t acc = 0u;
    uint32_t med_ns = min_ns;

    for (uint32_t b = 0u; b < HIST_BINS; b++) {
        acc += hist[b];
        if (acc > target) {
            med_ns = b * HIST_BIN_NS + HIST_BIN_NS / 2u;
            break;
        }
    }

    st->valid = true;
    st->periods = count;
    st->sample_mhz = 1000.0f / sample_ns;
    st->min_period_us = (float)min_ns / 1000.0f;
    st->med_period_us = (float)med_ns / 1000.0f;
    st->max_period_us = (float)max_ns / 1000.0f;
    st->min_low_us = (min_low_ns == UINT32_MAX) ? 0.0f
                                                : (float)min_low_ns / 1000.0f;
    st->max_low_us = (float)max_low_ns / 1000.0f;
    st->khz_fast = (min_ns != 0u) ? (1e6f / (float)min_ns) : 0.0f;
    st->khz_med = (med_ns != 0u) ? (1e6f / (float)med_ns) : 0.0f;
    return true;
}
