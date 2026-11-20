#ifndef TESTER_SCL_PROBE_H
#define TESTER_SCL_PROBE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     valid;
    uint32_t periods;       /* periods used for the statistics */
    float    sample_mhz;
    float    min_period_us;
    float    med_period_us;
    float    max_period_us;
    float    min_low_us;
    float    max_low_us;
    float    khz_fast;      /* 1 / shortest period */
    float    khz_med;       /* 1 / median period */
} scl_stats_t;

void     scl_probe_init(unsigned pin);
void     scl_probe_start(void);
uint32_t scl_probe_stop(void);
bool     scl_probe_analyze(uint32_t words, scl_stats_t *st);

#endif /* TESTER_SCL_PROBE_H */
