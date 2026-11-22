#include "tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crc8.h"
#include "i2c_util.h"
#include "pico/stdlib.h"
#include "regmap.h"

static uint32_t rng_state = 0x12345678u;
static char     g_last_why[80];
static char     g_val_why[80];
static char     g_last_rom[40];
static uint32_t g_transport;
static uint32_t g_validate;
static uint32_t g_retries;

/* Retry diagnostics: where in the cycle and where in the 256-byte map the
 * failure happened.  The first few events are kept verbatim; the rest only
 * feed the histograms. */
#define RETRY_SAMPLES   8
#define OFF_BUCKETS     16

typedef struct {
    uint8_t  cls;       /* 0 transport, 1 validate */
    uint8_t  attempt;
    uint8_t  flags;
    uint8_t  off;
    uint16_t cycle;
    uint16_t ms;
} retry_ev_t;

static retry_ev_t g_ev[RETRY_SAMPLES];
static int      g_ev_n;static uint32_t g_phase_busy;
static uint32_t g_phase_idle;
static uint32_t g_phase_unknown;
static uint32_t g_off_bucket[OFF_BUCKETS];
static uint32_t g_off_unknown;
static uint32_t g_mgr[8];       /* retries per manager state */
static uint32_t g_bit[8];       /* which bit position was corrupted */
static uint32_t g_step[3];      /* transport: seq-in, map, seq-out */

/* Snapshot of the last validate failure, for the anomaly investigation. */
static uint8_t g_fail_map[16];
static uint8_t g_fail_crc[2];
static uint8_t g_fail_seq[2];
static uint8_t g_fail_diag[4];
static uint8_t g_fail_trunc[2];
static uint8_t g_fail_ffstart;
static uint8_t g_fail_health;
static uint8_t g_fail_tail;
static uint32_t g_fail_frames;
static uint32_t g_fail_txn;
static uint8_t g_fail_valid;

/* Bytes that legitimately change between two reads. */
static bool map_volatile_index(int i)
{
    return (i == (int)REG_FLAGS) || (i == (int)REG_SEQ) ||
           ((i >= (int)REG_FRAMES) && (i <= (int)REG_SPEED)) ||
           ((i >= (int)REG_CMD) && (i <= (int)REG_DIAG_REASON)) ||
        ((i >= (int)REG_MAX_STEP) && (i <= (int)REG_EEP_POLL));
}

static void retry_stats_reset(void)
{
    g_ev_n = 0;
    g_phase_busy = 0u;
    g_phase_idle = 0u;
    g_phase_unknown = 0u;
    g_off_unknown = 0u;
    memset(g_off_bucket, 0, sizeof(g_off_bucket));
    memset(g_mgr, 0, sizeof(g_mgr));
    memset(g_bit, 0, sizeof(g_bit));
    memset(g_step, 0, sizeof(g_step));
    memset(g_ev, 0, sizeof(g_ev));
}

static void retry_capture(int cls, int attempt, uint32_t us, int off,
                          uint8_t xor_bits, int step)
{
    uint8_t flags = 0;
    uint8_t raw[2] = { 0, 0 };
    uint8_t mgr = 0xFF;
    bool got_flags = bus_read_reg(REG_FLAGS, &flags, 1u) == 1;
    uint16_t cycle = 0;

    (void)bus_read_reg(REG_MGR, &mgr, 1u);

    if (bus_read_reg(REG_CYCLE_L, raw, 2u) == 2) {
        cycle = (uint16_t)(raw[0] | ((uint16_t)raw[1] << 8));
    }

    if (!got_flags) {
        g_phase_unknown++;
    } else if ((flags & FLAG_BUSY) != 0u) {
        g_phase_busy++;
    } else {
        g_phase_idle++;
    }

    if (mgr < 8u) {
        g_mgr[mgr]++;
    }
    if ((step >= 0) && (step < 3)) {
        g_step[step]++;
    }
    for (int b = 0; b < 8; b++) {
        if ((xor_bits & (1u << b)) != 0u) {
            g_bit[b]++;
        }
    }

    if ((off >= 0) && (off < 256)) {
        g_off_bucket[off / (256 / OFF_BUCKETS)]++;
    } else {
        g_off_unknown++;
    }

    if (g_ev_n < RETRY_SAMPLES) {
        retry_ev_t *e = &g_ev[g_ev_n++];

        e->cls = (uint8_t)cls;
        e->attempt = (uint8_t)attempt;
        e->flags = got_flags ? flags : 0xFFu;
        e->off = ((off >= 0) && (off < 256)) ? (uint8_t)off : 0xFFu;
        e->cycle = cycle;
        e->ms = (uint16_t)((us > 65535000u) ? 65535u : (us / 1000u));
    }
}

static void retry_stats_print(void)
{
    static const char *const state_name[6] = {
        "search", "convert", "read", "eeprom", "publish", "wait"
    };

    printf("retry phase: busy %u, idle %u, unknown %u\n",
           (unsigned)g_phase_busy, (unsigned)g_phase_idle,
           (unsigned)g_phase_unknown);
    printf("retry manager:");
    for (int i = 0; i < 6; i++) {
        printf(" %s %u", state_name[i], (unsigned)g_mgr[i]);
    }
    printf("\nretry transport steps: seq-in %u, map %u, seq-out %u\n",
           (unsigned)g_step[0], (unsigned)g_step[1], (unsigned)g_step[2]);
    printf("retry bit positions:");
    for (int i = 0; i < 8; i++) {
        printf(" b%d %u", i, (unsigned)g_bit[i]);
    }
    printf("\nretry offsets:");
    for (int i = 0; i < OFF_BUCKETS; i++) {
        printf(" %u", (unsigned)g_off_bucket[i]);
    }
    printf(" unknown %u\n", (unsigned)g_off_unknown);

    for (int i = 0; i < g_ev_n; i++) {
        printf("  #%d %s try%d cycle %u flags 0x%02X off %u %u ms\n", i + 1,
               g_ev[i].cls ? "validate" : "transport", g_ev[i].attempt + 1,
               g_ev[i].cycle, g_ev[i].flags, g_ev[i].off,
               (unsigned)g_ev[i].ms);
    }
}

static uint32_t rnd(void)
{
    uint32_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static int read_map_checked(uint32_t *torn, uint32_t *invalid,
                            uint32_t *max_us)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t map[256];
        uint8_t s1;
        uint8_t s2;
        absolute_time_t t0 = get_absolute_time();
        int step = 0;

        if (bus_read_reg(REG_SEQ, &s1, 1u) != 1) {
            step = 1;
        } else if (!map_read(map)) {
            step = 2;
        } else if (bus_read_reg(REG_SEQ, &s2, 1u) != 1) {
            step = 3;
        }

        int64_t us = absolute_time_diff_us(t0, get_absolute_time());

        if (step != 0) {
            snprintf(g_last_why, sizeof(g_last_why), "i2c:%d", step);
            g_transport++;
            g_retries++;
            retry_capture(0, attempt, (uint32_t)us, -1, 0u, step - 1);
            continue;
        }

        if ((uint32_t)us > *max_us) {
            *max_us = (uint32_t)us;
        }

        if ((s1 != s2) || ((s1 & 1u) != 0u)) {
            (*torn)++;
            return 1;
        }

        char why[80];

        if (map_validate(map, why, sizeof(why))) {
            return 0;
        }

        /* A stable snapshot that fails validation is a payload bit error.
         * Two consecutive clean reads give a reference; only if they agree is
         * the difference a real corruption and not a legitimate update. */
        int off = -1;
        uint8_t xb = 0u;
        {
            memcpy(g_fail_map, map, sizeof(g_fail_map));
            g_fail_crc[0] = map[REG_MAP_CRC_L];
            g_fail_crc[1] = map[REG_MAP_CRC_H];
            g_fail_seq[0] = s1;
            g_fail_seq[1] = s2;
            (void)bus_read_reg(REG_DIAG_CRC_FFFF, g_fail_diag,
                               sizeof(g_fail_diag));
            (void)bus_read_reg(REG_OUF_POS, g_fail_trunc,
                               sizeof(g_fail_trunc));
            {
                uint8_t fr[4];

                g_fail_txn = bus_txn;
                g_fail_frames = 0u;
                if (bus_read_reg(REG_FRAMES, fr, sizeof(fr)) ==
                    (int)sizeof(fr)) {
                    g_fail_frames = (uint32_t)fr[0] |
                                    ((uint32_t)fr[1] << 8) |
                                    ((uint32_t)fr[2] << 16) |
                                    ((uint32_t)fr[3] << 24);
                }
            }
            {
                uint8_t h = 0u;
                uint8_t tail = 0u;

                (void)bus_read_reg(REG_HEALTH, &h, 1u);
                g_fail_health = h;
                g_fail_ffstart = 0xFFu;
                for (int i = 0; i <= 240; i++) {
                    bool all = true;

                    for (int j = 0; j < 16; j++) {
                        if (map[i + j] != 0xFFu) {
                            all = false;
                            break;
                        }
                    }
                    if (all) {
                        g_fail_ffstart = (uint8_t)i;
                        break;
                    }
                }
                for (int i = 255; i >= 0; i--) {
                    if (map[i] != 0xFFu) {
                        break;
                    }
                    tail++;
                }
                g_fail_tail = tail;
            }
            g_fail_valid = 1u;
        }
        {
            uint8_t map2[256];
            uint8_t map3[256];

            if (map_read(map2) && map_read(map3)) {
                bool same = true;

                for (int i = 0; (i < 256) && same; i++) {
                    if (!map_volatile_index(i) && (map2[i] != map3[i])) {
                        same = false;
                    }
                }
                if (same) {
                    for (int i = 0; i < 256; i++) {
                        if (!map_volatile_index(i) && (map[i] != map2[i])) {
                            off = i;
                            xb = (uint8_t)(map[i] ^ map2[i]);
                            break;
                        }
                    }
                }
            }
        }
        retry_capture(1, attempt, (uint32_t)us, off, xb, 0);

        strncpy(g_last_why, why, sizeof(g_last_why) - 1u);
        g_last_why[sizeof(g_last_why) - 1u] = '\0';
        strncpy(g_val_why, why, sizeof(g_val_why) - 1u);
        g_val_why[sizeof(g_val_why) - 1u] = '\0';
        int slot = 0;

        if (strncmp(why, "rom ", 4u) == 0) {
            slot = atoi(&why[4]);
        }
        if ((slot < 0) || (slot >= (int)DEV_MAX)) {
            slot = 0;
        }
        snprintf(g_last_rom, sizeof(g_last_rom),
                 "%02X %02X %02X %02X %02X %02X %02X %02X",
                 map[REG_ROMS + 8 * slot + 0], map[REG_ROMS + 8 * slot + 1],
                 map[REG_ROMS + 8 * slot + 2], map[REG_ROMS + 8 * slot + 3],
                 map[REG_ROMS + 8 * slot + 4], map[REG_ROMS + 8 * slot + 5],
                 map[REG_ROMS + 8 * slot + 6], map[REG_ROMS + 8 * slot + 7]);
        g_validate++;
        g_retries++;
    }

    (*invalid)++;
    return -1;
}

static void stress_loop(uint32_t count, int mode, const char *label)
{
    uint32_t torn = 0u;
    uint32_t invalid = 0u;
    uint32_t max_us = 0u;
    uint32_t good = 0u;
    uint32_t errors = 0u;
    uint8_t  ouf0[3] = { 0u, 0u, 0u };
    uint8_t  ouf1[3] = { 0u, 0u, 0u };

    g_abort = false;
    bus_stats_reset();
    (void)bus_read_reg(REG_OUF_END, ouf0, sizeof(ouf0));
    g_transport = 0u;
    g_validate = 0u;
    g_retries = 0u;
    retry_stats_reset();
    g_last_why[0] = '\0';
    g_val_why[0] = '\0';
    g_last_rom[0] = '\0';

    for (uint32_t i = 0u; i < count; i++) {
        int r;

        if (mode == 1) {
            (void)bus_fault(FAULT_EDGES);
        } else if (mode == 2) {
            (void)bus_fault(FAULT_STALL);
        }
        r = read_map_checked(&torn, &invalid, &max_us);

        if (r < 0) {
            errors++;
        } else if (r == 0) {
            good++;
        }
        if ((i % 128u) == 0u) {
            service();
            if (g_abort) {
                break;
            }
        }
    }
    printf("%s %u: %u good, %u torn, %u invalid, %u errors, max %u us\n",
           label, count, good, torn, invalid, errors, max_us);
    printf("errors: %u timeout, %u nack, %u other; transport %u, validate %u, retries %u, last: %s, val: %s [%s]\n",
           (unsigned)bus_err_timeout, (unsigned)bus_err_nack,
           (unsigned)bus_err_other, (unsigned)g_transport,
           (unsigned)g_validate, (unsigned)g_retries, g_last_why, g_val_why,
           g_last_rom);
    retry_stats_print();

    {
        uint8_t ms[7];
        uint8_t f[13];

        if (bus_read_reg(REG_MAX_STEP, ms, sizeof(ms)) == (int)sizeof(ms)) {
            printf("max manager step: %u us, state %u; ouf %u@%u; "
                   "eep start %u us, poll %u us\n",
                   (unsigned)((uint16_t)ms[0] | ((uint16_t)ms[1] << 8)), ms[2],
                   ms[3], ms[4], ms[5], ms[6]);
        }
        if (bus_read_reg(REG_OUF_END, f, sizeof(f)) == (int)sizeof(f)) {
            printf("ouf end %u mid %u late %u age %u us; sts1 %02X sts2 %02X "
                   "pos %u ptr %02X; slow %u us state %u sub %u\n",
                   f[0], f[1], f[2], f[5], f[3], f[4], f[6], f[7],
                   f[9], f[10], f[11]);
            printf("max sub %u\n", f[12]);
        }
    }

    if (g_fail_valid != 0u) {
        printf("last fail: seq %02X/%02X magic %02X ver %02X flags %02X "
               "crc %02X%02X diag ffff %u busy %u ack %u reason %u health %02X "
               "ff-tail %u ff-start %u ouf %u@%u frames %u txn %u\n",
               g_fail_seq[0], g_fail_seq[1], g_fail_map[0], g_fail_map[1],
               g_fail_map[2], g_fail_crc[1], g_fail_crc[0], g_fail_diag[0],
               g_fail_diag[1], g_fail_diag[2], g_fail_diag[3], g_fail_health,
               g_fail_tail, g_fail_ffstart, g_fail_trunc[0], g_fail_trunc[1],
               (unsigned)g_fail_frames, (unsigned)g_fail_txn);
    }

    /* Zero tolerance: an underrun, a retry or a malformed map is a failure,
     * not noise.  A torn read is the host racing a republish, which the retry
     * already re-read; it is not a corruption. */
    (void)bus_read_reg(REG_OUF_END, ouf1, sizeof(ouf1));
    {
        bool clean = (g_retries == 0u) && (invalid == 0u) && (errors == 0u) &&
                     (ouf1[0] == ouf0[0]) && (ouf1[1] == ouf0[1]) &&
                     (ouf1[2] == ouf0[2]);

        printf("RESULT: %s\n", clean ? "PASS" : "FAIL");
        if (!clean) {
            printf("RESULT DETAIL: retries %u invalid %u errors %u "
                   "ouf end +%u mid +%u late +%u\n",
                   (unsigned)g_retries, (unsigned)invalid, (unsigned)errors,
                   (unsigned)(uint8_t)(ouf1[0] - ouf0[0]),
                   (unsigned)(uint8_t)(ouf1[1] - ouf0[1]),
                   (unsigned)(uint8_t)(ouf1[2] - ouf0[2]));
        }
    }
}

void stress_run(uint32_t count)
{
    g_fail_valid = 0u;
    stress_loop(count, 0, "stress");
}

/* Narrow reproducer: read one register block over and over, so an underrun
 * can be tied to the block length and to the state of the manager. */
/* One live register, retried: a multi-byte read of the live block can itself
 * be perturbed by the very underrun it is measuring. */
static int read_u8(uint8_t reg, uint8_t *v)
{
    for (int i = 0; i < 3; i++) {
        if (bus_read_reg(reg, v, 1u) == 1) {
            return 1;
        }
    }
    return 0;
}

static void print_diag(void)
{
    static const uint8_t dregs[23] = {
        REG_DT_FULL, REG_OUF_END, REG_OUF_MID, REG_ISR_MAX,
        REG_OUF_STS1, REG_OUF_STS2, REG_OUF_AGE, REG_ADDR_DT,
        REG_OUF_REG_PTR, REG_SLOW_COUNT, REG_SLOW_US, REG_SLOW_STATE,
        REG_SLOW_SUB, REG_MAX_SUB, REG_ACT_PREV, REG_ACT_DT,
        REG_ACT_PREV2, REG_ACT_DT2, REG_ADDR_STALE, REG_POLL_GAP_L,
        REG_POLL_GAP_H, REG_POLL_GAP_STATE, REG_POLL_GAP_SUB
    };
    uint8_t v[23];

    for (int i = 0; i < 23; i++) {
        if (!read_u8(dregs[i], &v[i])) {
            v[i] = 0xFFu;
        }
    }
    printf("diag dtfull %u end %u mid %u isr %u us; ouf age %u sts1 %02X "
           "sts2 %02X addr_dt %u us ptr %02X\n",
           v[0], v[1], v[2], v[3], v[6], v[4], v[5], v[7], v[8]);
    printf("slow %u us state %u sub %u maxsub %u; addr stale %u; "
           "poll gap %u us state %u sub %u\n",
           v[10], v[11], v[12], v[13], v[18],
           (unsigned)((uint16_t)v[19] | ((uint16_t)v[20] << 8)), v[21], v[22]);
}

void diag_run(void)
{
    print_diag();
}

void repeat_run(uint8_t reg, uint32_t len, uint32_t n)
{
    uint8_t buf[256];
    uint32_t good = 0u;
    uint32_t wbad = 0u;
    uint32_t rbad = 0u;
    uint32_t max_us = 0u;
    uint32_t diff = 0u;
    uint8_t ref[256];
    uint8_t diff_val = 0u;
    uint8_t ref_val = 0u;
    int have_ref = 0;
    uint32_t t0err = bus_err_timeout;
    uint32_t n0err = bus_err_nack;
    uint32_t o0err = bus_err_other;

    if ((len == 0u) || (len > sizeof(buf)) || (n == 0u)) {
        printf("usage: repeat <reg> <len 1..256> <count>\n");
        return;
    }
    for (uint32_t i = 0u; i < n; i++) {
        absolute_time_t t0 = get_absolute_time();
        int w = bus_write_reg(reg, NULL, 0u);   /* pointer write */
        int r = (w == 0) ? bus_read_only(buf, (size_t)len) : -1;
        uint32_t us = (uint32_t)absolute_time_diff_us(t0, get_absolute_time());

        if (us > max_us) {
            max_us = us;
        }
        if (w != 0) {
            wbad++;
        } else if (r != (int)len) {
            rbad++;
        } else {
            good++;
            /* The first reads can precede the first published snapshot (the
             * map is zero until the manager runs), so the reference is taken
             * only after a few clean reads. */
            if (!have_ref) {
                if (good >= 8u) {
                    memcpy(ref, buf, (size_t)len);
                    have_ref = 1;
                }
            } else if (memcmp(ref, buf, (size_t)len) != 0) {
                if (diff == 0u) {
                    for (uint32_t k = 0u; k < len; k++) {
                        if (ref[k] != buf[k]) {
                            ref_val = ref[k];
                            diff_val = buf[k];
                            break;
                        }
                    }
                }
                diff++;
            }
        }
        if ((i % 256u) == 0u) {
            service();
        }
    }
    printf("repeat 0x%02X x %u x %u: %u good, %u wbad, %u rbad, %u diff, "
           "max %u us; first %02X last %02X\n",
           reg, (unsigned)len, (unsigned)n, (unsigned)good, (unsigned)wbad,
           (unsigned)rbad, (unsigned)diff, (unsigned)max_us,
           (unsigned)(have_ref ? ref[0] : 0), (unsigned)buf[0]);
    printf("bus err +%u timeout, +%u nack, +%u other\n",
           (unsigned)(bus_err_timeout - t0err),
           (unsigned)(bus_err_nack - n0err),
           (unsigned)(bus_err_other - o0err));
    print_diag();
}

/* Every read is preceded by a deliberate 1-Wire edge burst, so the crosstalk
 * hypothesis can be tested in seconds.  Run it with the manager paused. */
void couple_run(uint32_t count)
{
    stress_loop(count, 1, "couple");
}

/* Every read is stalled for 200 us at byte 64, simulating the CPU time the
 * manager takes between I2C transfers. */
void stall_run(uint32_t count)
{
    stress_loop(count, 2, "stall");
}

void suite_stress(void)
{
    uint8_t map[256];
    uint8_t alt[256];
    char why[80];

    t_start("T35", "10k map reads");
    {
        uint32_t torn = 0u;
        uint32_t invalid = 0u;
        uint32_t max_us = 0u;
        uint32_t good = 0u;
        uint32_t errors = 0u;

        for (uint32_t i = 0u; i < 10000u; i++) {
            int r = read_map_checked(&torn, &invalid, &max_us);

            if (r < 0) {
                errors++;
            } else if (r == 0) {
                good++;
            }
            if ((i % 128u) == 0u) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if ((errors == 0u) && (invalid == 0u) && (good >= 9900u)) {
            t_pass("%u good, %u torn, max %u us", good, torn, max_us);
        } else {
            t_fail("%u good, %u torn, %u invalid, %u errors", good, torn,
                   invalid, errors);
        }
    }

    static char soak_label[32];

    snprintf(soak_label, sizeof(soak_label), "%u s soak",
             (unsigned)g_soak_seconds);
    t_start("T36", soak_label);
    {
        uint32_t torn = 0u;
        uint32_t invalid = 0u;
        uint32_t max_us = 0u;
        uint32_t good = 0u;
        uint32_t errors = 0u;

        g_retries = 0u;
        retry_stats_reset();
        absolute_time_t deadline = make_timeout_time_ms(g_soak_seconds * 1000u);

        while (absolute_time_diff_us(get_absolute_time(), deadline) > 0) {
            int r = read_map_checked(&torn, &invalid, &max_us);

            if (r < 0) {
                errors++;
            } else if (r == 0) {
                good++;
            }
            service();
            if (g_abort) {
                break;
            }
        }
        if ((errors == 0u) && (invalid == 0u) && (good > 0u)) {
            t_pass("%u good, %u torn, %u retries, max %u us", good, torn,
                   (unsigned)g_retries, max_us);
        } else {
            t_fail("%u good, %u torn, %u invalid, %u errors", good, torn,
                   invalid, errors);
        }
        retry_stats_print();
    }

    t_start("T37", "random reads");
    {
        uint32_t errors = 0u;

        for (int i = 0; i < 4096; i++) {
            uint8_t buf[16];
            uint32_t r = rnd();
            uint8_t reg = (uint8_t)(r & 0xFFu);
            size_t len = 1u + ((r >> 8) % 16u);
            bool ok = false;

            for (int attempt = 0; attempt < 3; attempt++) {
                if (bus_read_reg(reg, buf, len) == (int)len) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                errors++;
            }
            if ((i % 256) == 0) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if ((errors == 0u) && map_stable(map, 3000u) &&
            (map[REG_MAGIC] == 0x5Au)) {
            t_pass("4096 reads");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T38", "random read-only writes");
    {
        uint8_t rom0[8];
        uint8_t eep[8];
        uint8_t dev = 0u;
        uint8_t ds = 0u;
        uint32_t errors = 0u;

        map_stable(map, 3000u);
        memcpy(rom0, &map[REG_ROMS], 8u);
        memcpy(eep, &map[REG_EEPROM], 8u);
        dev = map[REG_DEVICES];
        ds = map[REG_DS18B20];

        for (int i = 0; i < 1024; i++) {
            uint32_t r = rnd();
            uint8_t reg = (uint8_t)(r & 0xFFu);
            uint8_t v = (uint8_t)(r >> 8);

            if ((reg == REG_SPEED) || (reg == REG_CMD) ||
                (reg == REG_SELECTED)) {
                continue;
            }
            if (bus_write_reg(reg, &v, 1u) != 1) {
                bool ok = false;

                for (int attempt = 0; attempt < 3; attempt++) {
                    if (bus_write_reg(reg, &v, 1u) == 1) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    errors++;
                }
            }
        }

        if ((errors == 0u) && map_stable(alt, 3000u) &&
            (alt[REG_MAGIC] == 0x5Au) && (alt[REG_DEVICES] == dev) &&
            (alt[REG_DS18B20] == ds) &&
            (memcmp(&alt[REG_ROMS], rom0, 8u) == 0) &&
            (memcmp(&alt[REG_EEPROM], eep, 8u) == 0)) {
            t_pass("snapshot intact");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T39", "mixed fuzz");
    {
        uint32_t errors = 0u;

        for (int i = 0; i < 1000; i++) {
            uint32_t r = rnd();
            uint8_t buf[16];
            bool ok = false;

            for (int attempt = 0; attempt < 3 && !ok; attempt++) {
                switch (r & 7u) {
                case 0u:
                    ok = bus_read_reg(REG_MAGIC, buf, 16u) == 16;
                    break;
                case 1u: {
                    uint8_t full[256];

                    ok = map_read(full);
                    break;
                }
                case 2u:
                    ok = send_cmd(CMD_CLEAR_COUNTERS);
                    break;
                case 3u:
                    ok = send_cmd(0x7Eu);
                    break;
                case 4u:
                    ok = !bus_probe(0x50u);
                    break;
                case 5u:
                    ok = bus_read_reg(REG_SCRATCH, buf, 9u) == 9;
                    break;
                case 6u: {
                    uint8_t v = 7u;

                    ok = bus_write_reg(REG_SPEED, &v, 1u);
                    break;
                }
                default:
                    ok = bus_probe(BRIDGE_ADDR);
                    break;
                }
            }
            if (!ok) {
                errors++;
            }

            if ((i % 100) == 99) {
                if (!map_stable(alt, 3000u) ||
                    !map_validate(alt, why, sizeof(why))) {
                    errors++;
                }
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if (errors == 0u) {
            t_pass("1000 mixed ops");
        } else {
            t_fail("%u errors", errors);
        }
    }

    t_start("T40", "nack recovery");
    {
        int errors = 0;

        for (int i = 0; i < 100; i++) {
            if (bus_probe(0x50u)) {
                errors++;
            }
        }
        if ((errors == 0) && map_stable(map, 3000u) &&
            map_validate(map, why, sizeof(why))) {
            t_pass("100 nacks, map ok");
        } else {
            t_fail("%d unexpected acks", errors);
        }
    }

    t_start("T41", "nack storm");
    {
        int errors = 0;

        for (int i = 0; i < 1000; i++) {
            bool bad = true;

            for (int attempt = 0; attempt < 3; attempt++) {
                bool nack50 = bus_probe(0x50u);
                bool ack42 = bus_probe(BRIDGE_ADDR);

                if (!nack50 && ack42) {
                    bad = false;
                    break;
                }
            }
            if (bad) {
                errors++;
            }
            if ((i % 200) == 0) {
                service();
                if (g_abort) {
                    break;
                }
            }
        }
        if (errors == 0) {
            t_pass("1000 alternations");
        } else {
            t_fail("%d errors", errors);
        }
    }

    t_start("T42", "final validation");
    {
        why[0] = '\0';
        bool ok = map_stable(map, 3000u) &&
                  map_validate(map, why, sizeof(why));
        uint16_t cycle = ok ? map_cycle(map) : 0u;

        sleep_ms(1500u);
        ok = ok && map_stable(alt, 3000u) &&
             (map_cycle(alt) != cycle);

        if (ok) {
            t_pass("cycle advancing, map valid");
        } else {
            t_fail("%s", why);
        }
    }

    bus_set_speed(BUS_SPEED_400K);
    write_speed(1u);
}














