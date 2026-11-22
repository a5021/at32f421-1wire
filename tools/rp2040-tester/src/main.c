#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "crc8.h"
#include "i2c_util.h"
#include "lineedit.h"
#include "regmap.h"
#include "scl_probe.h"
#include "tests.h"

#ifndef TESTER_VERSION
#define TESTER_VERSION "1.0"
#endif
#ifndef TESTER_SDK_VERSION
#define TESTER_SDK_VERSION "unknown"
#endif
#ifndef TESTER_BOARD
#define TESTER_BOARD "pico"
#endif

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

#define LED_PIN             PICO_DEFAULT_LED_PIN
#define SAMPLER_PIN         5u

void suite_bus(void);
void suite_map(void);
void suite_ow(void);
void suite_stress(void);
void suite_fault(void);
int  fuzz_run(uint32_t seed, uint32_t ops, uint32_t *digest);
void stress_run(uint32_t count);
void repeat_run(uint8_t reg, uint32_t len, uint32_t n);
void diag_run(void);
void couple_run(uint32_t count);
void stall_run(uint32_t count);

static lineedit_t editor;

static bool     running;
static int      fail_code;
static uint64_t led_next_us;
static bool     led_state;
static int      fail_step;
static uint64_t fail_next_us;
static bool     adc_ready;


static void seed_history(lineedit_t *le);
static const char *health_verdict(void);

static void led_poll(void)
{
    uint64_t now = time_us_64();

    if (running) {
        if (now >= led_next_us) {
            led_state = !led_state;
            gpio_put(LED_PIN, led_state);
            led_next_us = now + 200000u;
        }
        return;
    }

    if (fail_code == 0) {
        gpio_put(LED_PIN, 1);
        return;
    }

    if (now >= fail_next_us) {
        if (fail_step >= 2 * fail_code) {
            fail_step = 0;
            fail_next_us = now + 1200000u;
            gpio_put(LED_PIN, 0);
        } else {
            gpio_put(LED_PIN, (fail_step % 2) == 0);
            fail_step++;
            fail_next_us = now + 200000u;
        }
    }
}

void service(void)
{
    int c;

    led_poll();
    while ((c = getchar_timeout_us(0)) >= 0) {
        if (c == 3) {                    /* Ctrl-C only: arrows must not abort */
            if (!g_abort) {
                printf("\n[abort requested]\n");
            }
            g_abort = true;
        }
    }
}

static const char *family_name(uint8_t family)
{
    switch (family) {
    case FAMILY_DS18B20:
        return "DS18B20";
    case FAMILY_DS2431:
        return "DS2431";
    case FAMILY_DS2413:
        return "DS2413";
    default:
        return "unknown";
    }
}

static const char *st_text(uint8_t st)
{
    static const char *const names[] = {
        "ok", "busy", "no presence", "crc error", "param",
        "bus error", "end"
    };

    return (st < sizeof(names) / sizeof(names[0])) ? names[st] : "-";
}

static void print_temp(uint16_t raw)
{
    if (raw == INVALID_TEMP) {
        printf("      -");
        return;
    }

    int32_t milli = (int32_t)(int16_t)raw * 1000 / 16;

    if (milli < 0) {
        printf("-%d.%03d", (int)(-milli / 1000), (int)(-milli % 1000));
    } else {
        printf("%d.%03d", (int)(milli / 1000), (int)(milli % 1000));
    }
}

static void print_us(uint32_t us)
{
    printf("%u.%03u", us / 1000u, us % 1000u);
}

static int first_failure_suite(void)
{
    for (int i = 0; i < g_result_count; i++) {
        if (!g_results[i].ok) {
            int n = atoi(&g_results[i].id[1]);

            if (n <= 11) {
                return 1;
            }
            if (n <= 24) {
                return 2;
            }
            if (n <= 34) {
                return 3;
            }
            return 4;
        }
    }
    return 0;
}

static void print_results(void)
{
    for (int i = 0; i < g_result_count; i++) {
        printf("%s %-22s %s%s%s\n", g_results[i].id, g_results[i].name,
               g_results[i].ok ? "PASS" : "FAIL",
               g_results[i].detail[0] ? " - " : "", g_results[i].detail);
    }
}

static int count_failures(void)
{
    int fail = 0;

    for (int i = 0; i < g_result_count; i++) {
        if (!g_results[i].ok) {
            fail++;
        }
    }
    return fail;
}

static const char *last_failure(void)
{
    for (int i = g_result_count - 1; i >= 0; i--) {
        if (!g_results[i].ok) {
            return g_results[i].id;
        }
    }
    return "-";
}

static void print_info(void)
{
    printf("tester %s (sdk %s, board %s)\n", TESTER_VERSION,
           TESTER_SDK_VERSION, TESTER_BOARD);
    printf("built %s %s\n", __DATE__, __TIME__);
    printf("sys %u kHz, peri %u kHz, usb %u kHz\n",
           (unsigned)(clock_get_hz(clk_sys) / 1000u),
           (unsigned)(clock_get_hz(clk_peri) / 1000u),
           (unsigned)(clock_get_hz(clk_usb) / 1000u));
    printf("i2c0 SDA GP4, SCL GP5, target 0x%02X\n", BRIDGE_ADDR);
    printf("sampler %u kHz (%u ns per sample)\n",
           (unsigned)(clock_get_hz(clk_sys) / 2000u),
           (unsigned)(2000000000u / clock_get_hz(clk_sys)));
    printf("history %d entries\n", lineedit_history_count(&editor));
}

static void print_status(void)
{
    uint8_t hdr[16];
    uint32_t up_s = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000u);
    bool ack = bus_probe(BRIDGE_ADDR);

    printf("uptime %u s, link %u kHz\n", up_s, (unsigned)bus_nominal_khz());
    printf("target 0x%02X: %s\n", BRIDGE_ADDR, ack ? "ack" : "nack");

    if (g_result_count != 0) {
        printf("last run: %d/%d passed, first fail %s\n",
               g_result_count - count_failures(), g_result_count,
               last_failure());
    } else {
        printf("last run: none\n");
    }

    if (ack && (bus_read_reg(REG_MAGIC, hdr, sizeof(hdr)) == (int)sizeof(hdr))) {
        printf("snapshot: %u devices, %u ds18b20, cycle %u, flags 0x%02X\n",
               hdr[REG_DEVICES], hdr[REG_DS18B20],
               (unsigned)(hdr[REG_CYCLE_L] | (hdr[REG_CYCLE_H] << 8)),
               hdr[REG_FLAGS]);
    } else {
        printf("snapshot: unavailable\n");
    }

    if (ack && bus_health_sample()) {
        printf("health: %s, %u txn, %u missed, %u truncated, flags 0x%02X\n",
               health_verdict(), (unsigned)bus_txn,
               (unsigned)bus_health_missed, (unsigned)bus_health_truncated,
               bus_health_flags);
    }
}

static void print_scan(void)
{
    int found = 0;

    printf("i2c scan 0x08..0x77:");
    for (uint8_t addr = 0x08u; addr <= 0x77u; addr++) {
        if (bus_probe(addr)) {
            printf(" %02X", addr);
            found++;
        }
        if ((addr & 0x0Fu) == 0x0Fu) {
            service();
        }
    }
    if (found == 0) {
        printf(" none");
    }
    printf(" (%d)\n", found);
}

static void cmd_temp(const char *arg)
{
    uint8_t map[256];

    if (!map_stable(map, 3000u)) {
        printf("no snapshot\n");
        return;
    }

    int want = (arg != NULL && arg[0] != '\0') ? atoi(arg) : -1;
    int ds = map[REG_DS18B20];
    int idx = 0;

    if (ds == 0) {
        printf("no DS18B20 found\n");
        return;
    }
    if ((want >= ds) || (want < -1)) {
        printf("sensor %d out of range (0..%d)\n", want, ds - 1);
        return;
    }

    for (int i = 0; (i < map_stored_devices(map)) && (idx < ds); i++) {
        const uint8_t *rom = &map[REG_ROMS + 8 * i];

        if (rom[0] != FAMILY_DS18B20) {
            continue;
        }
        if ((want < 0) || (want == idx)) {
            uint16_t raw = (uint16_t)(map[REG_TEMP + 2 * idx] |
                                      (map[REG_TEMP + 2 * idx + 1] << 8));
            uint8_t st = map[REG_TEMP_STATUS + idx];

            printf("#%d ", idx);
            print_temp(raw);
            printf(" C  %-11s rom %02X%02X%02X%02X%02X%02X%02X%02X\n",
                   st_text(st), rom[0], rom[1], rom[2], rom[3], rom[4],
                   rom[5], rom[6], rom[7]);
        }
        idx++;
    }
}

static void cmd_roms(void)
{
    uint8_t map[256];

    if (!map_stable(map, 3000u)) {
        printf("no snapshot\n");
        return;
    }

    int stored = map_stored_devices(map);
    int empty = 0;

    for (int i = 0; i < stored; i++) {
        const uint8_t *rom = &map[REG_ROMS + 8 * i];
        bool blank = true;

        for (int b = 0; b < 8; b++) {
            if (rom[b] != 0xFFu) {
                blank = false;
            }
        }
        if (blank) {
            empty++;
            continue;
        }
        uint8_t crc = crc8_dallas(rom, 7u);

        printf("#%-2d %02X %02X %02X %02X %02X %02X %02X %02X  %-8s crc %s\n",
               i, rom[0], rom[1], rom[2], rom[3], rom[4], rom[5], rom[6],
               rom[7], family_name(rom[0]), crc == rom[7] ? "ok" : "BAD");
    }
    printf("%u devices, %u ds18b20, %d empty slots\n", map[REG_DEVICES],
           map[REG_DS18B20], empty);
}

static void cmd_scratch(const char *arg)
{
    uint8_t map[256];
    uint8_t alt[256];
    uint8_t sp[9];
    int n = (arg != NULL && arg[0] != '\0') ? atoi(arg) : 0;

    if (!map_stable(map, 3000u)) {
        printf("no snapshot\n");
        return;
    }
    if ((n < 0) || (n >= (int)map[REG_DS18B20])) {
        printf("sensor %d out of range (0..%d)\n", n,
               (int)map[REG_DS18B20] - 1);
        return;
    }

    select_sensor((uint8_t)n);
    uint16_t cycle = map_cycle(map);

    printf("selecting %d, waiting for a cycle...\n", n);
    for (int i = 0; i < 2; i++) {
        if (!map_wait_cycle(cycle, alt, 8000u)) {
            printf("timeout\n");
            select_sensor(0u);
            return;
        }
        cycle = map_cycle(alt);
    }
    if (!read_scratch(sp)) {
        printf("read failed\n");
        select_sensor(0u);
        return;
    }

    uint16_t raw = (uint16_t)(sp[0] | ((uint16_t)sp[1] << 8));
    uint8_t crc = crc8_dallas(sp, 8u);
    int res = 9 + ((sp[4] >> 5) & 3);

    printf("raw %04X  ", raw);
    print_temp(raw);
    printf(" C  TH %d  TL %d  cfg 0x%02X (%d bit)  crc %s\n",
           (int8_t)sp[2], (int8_t)sp[3], sp[4], res,
           crc == sp[8] ? "ok" : "BAD");
    select_sensor(0u);
}

static void cmd_eeprom(void)
{
    uint8_t map[256];

    if (!map_stable(map, 3000u)) {
        printf("no snapshot\n");
        return;
    }

    printf("DS2431:");
    for (int i = 0; i < 8; i++) {
        printf(" %02X", map[REG_EEPROM + i]);
    }
    printf("  status %s\n", st_text(map[REG_EEPROM_STATUS]));
}

static void cmd_dump(const char *arg)
{
    uint8_t map[256];
    int start = 0;
    int len = 96;

    if ((arg != NULL) && (arg[0] != '\0')) {
        (void)sscanf(arg, "%i %i", &start, &len);
    }
    if ((start < 0) || (start > 255) || (len < 0)) {
        printf("usage: dump [start] [len]\n");
        return;
    }
    if (start + len > 256) {
        len = 256 - start;
    }
    if (!map_stable(map, 3000u)) {
        printf("no snapshot\n");
        return;
    }

    for (int row = 0; row < len; row += 16) {
        printf("%02X:", start + row);
        for (int i = 0; i < 16; i++) {
            if (row + i < len) {
                printf(" %02X", map[start + row + i]);
            } else {
                printf("   ");
            }
        }
        printf("  ");
        for (int i = 0; i < 16 && row + i < len; i++) {
            uint8_t b = map[start + row + i];

            putchar((b >= 32u && b < 127u) ? (char)b : '.');
        }
        printf("\n");
    }
}

static void cmd_reg(const char *arg)
{
    if ((arg == NULL) || (arg[0] == '\0')) {
        printf("usage: reg <addr>\n");
        return;
    }
    uint32_t addr = (uint32_t)strtoul(arg, NULL, 0) & 0xFFu;
    uint8_t v;

    if (bus_read_reg((uint8_t)addr, &v, 1u) == 1) {
        printf("0x%02X = 0x%02X (%u)\n", addr, v, v);
    } else {
        printf("read failed\n");
    }
}

static void cmd_send_cmd(const char *arg)
{
    if ((arg == NULL) || (arg[0] == '\0')) {
        printf("usage: cmd <n>\n");
        return;
    }
    uint8_t v = (uint8_t)strtoul(arg, NULL, 0);

    printf(send_cmd(v) ? "command %u queued\n" : "command failed\n", v);
}

static void cmd_select(const char *arg)
{
    if ((arg == NULL) || (arg[0] == '\0')) {
        printf("usage: select <n>\n");
        return;
    }
    uint8_t v = (uint8_t)(strtoul(arg, NULL, 0) & 0x0Fu);

    printf(select_sensor(v) ? "selected %u\n" : "select failed\n", v);
}

static void cmd_probe(const char *arg)
{
    if ((arg == NULL) || (arg[0] == '\0')) {
        printf("usage: probe <addr>\n");
        return;
    }
    uint8_t addr = (uint8_t)(strtoul(arg, NULL, 0) & 0x7Fu);

    printf("0x%02X: %s\n", addr, bus_probe(addr) ? "ack" : "nack");
}

static void cmd_sample(const char *arg)
{
    uint8_t buf[64];
    scl_stats_t st;
    int len = (arg != NULL && arg[0] != '\0') ? atoi(arg) : 32;

    if (len < 1) {
        len = 32;
    }
    if (len > (int)sizeof(buf)) {
        len = (int)sizeof(buf);
    }

    scl_probe_start();
    int got = bus_read_reg(REG_ROMS, buf, (size_t)len);
    uint32_t words = scl_probe_stop();

    if (got != len) {
        printf("bus read failed\n");
        return;
    }
    if (!scl_probe_analyze(words, &st)) {
        printf("no SCL edges captured\n");
        return;
    }

    printf("%.1f kHz fast, %.1f kHz median, %u periods\n",
           st.khz_fast, st.khz_med, st.periods);
    printf("period min ");
    print_us((uint32_t)(st.min_period_us * 1000.0f + 0.5f));
    printf(" us, med ");
    print_us((uint32_t)(st.med_period_us * 1000.0f + 0.5f));
    printf(" us, max ");
    print_us((uint32_t)(st.max_period_us * 1000.0f + 0.5f));
    printf(" us\nmax low ");
    print_us((uint32_t)(st.max_low_us * 1000.0f + 0.5f));
    printf(" us\n");
}

static uint32_t bench_read(uint32_t master_hz, uint8_t slave_speed)
{
    uint8_t buf[64];

    bus_set_speed(BUS_SPEED_100K);
    write_speed(slave_speed);
    sleep_ms(30u);
    bus_set_speed(master_hz);
    sleep_ms(30u);

    absolute_time_t t0 = get_absolute_time();
    int got = bus_read_reg(REG_ROMS, buf, sizeof(buf));
    int64_t us = absolute_time_diff_us(t0, get_absolute_time());

    return (got == (int)sizeof(buf) && us > 0) ? (uint32_t)us : 0u;
}

static void cmd_bench(void)
{
    uint32_t us100 = bench_read(BUS_SPEED_100K, 0u);
    uint32_t us400 = bench_read(BUS_SPEED_400K, 1u);

    if ((us100 == 0u) || (us400 == 0u)) {
        printf("bench failed\n");
        return;
    }

    printf("64 bytes: 100 kHz ");
    print_us(us100);
    printf(" us, 400 kHz ");
    print_us(us400);
    printf(" us, ratio %u.%ux\n", (us100 * 10u) / us400,
           ((us100 * 10u) % us400) * 10u / us400);
}

static void cmd_speed_auto(void)
{
    bus_set_speed(BUS_SPEED_100K);
    write_speed(1u);
    sleep_ms(30u);
    bus_set_speed(BUS_SPEED_400K);
    sleep_ms(20u);

    if (bus_probe(BRIDGE_ADDR)) {

        printf("400 kHz works\n");
        return;
    }

    bus_set_speed(BUS_SPEED_100K);
    write_speed(0u);

    printf("400 kHz failed, using 100 kHz\n");
}

static const char *health_verdict(void)
{
    uint32_t bad = bus_health_missed + bus_health_truncated;

    if ((bad == 0u) && (bus_health_flags == 0u)) {
        return "CLEAN";
    }
    /* A stuck line, a real loss rate, or repeated bus errors are faults.  A
     * single bus error in tens of thousands of transactions is noise. */
    if (((bus_health_flags & HEALTH_SDA_STUCK) != 0u) ||
        ((bus_txn != 0u) && (bad * 100u > bus_txn)) ||
        (bus_health_err_bus >= 3u)) {
        return "BAD";
    }
    return "DEGRADED";
}

static void cmd_fault(const char *arg)
{
    uint8_t code = (uint8_t)strtoul(arg, NULL, 0);

    if (bus_fault(code)) {
        printf("fault %u armed\n", code);
    } else {
        printf("fault %u not armed\n", code);
    }
}

static void cmd_fuzz(const char *arg)
{
    uint32_t ops = 200u;
    uint32_t seed = time_us_32();
    uint32_t digest = 0u;

    if ((arg != NULL) && (arg[0] != '\0')) {
        ops = (uint32_t)strtoul(arg, NULL, 10);
        if (ops == 0u) {
            ops = 200u;
        }
        const char *p = strchr(arg, ' ');

        if (p != NULL) {
            seed = (uint32_t)strtoul(p + 1, NULL, 0);
        }
    }

    printf("fuzz %u ops, seed 0x%08X\n", (unsigned)ops, (unsigned)seed);
    int fails = fuzz_run(seed, ops, &digest);

    printf("digest 0x%08X, %d invariant failures\n", (unsigned)digest, fails);
}

static void cmd_health(const char *arg)
{
    if ((arg != NULL) && (strcmp(arg, "clear") == 0)) {
        if (!send_cmd(CMD_CLEAR_COUNTERS)) {
            printf("clear failed\n");
            return;
        }
        sleep_ms(10u);
        (void)send_cmd(CMD_STALL_RESET);
        bus_stats_reset();
        bus_health_reset();
        printf("health counters cleared\n");
        return;
    }

    if (!bus_health_sample()) {
        printf("health: read failed\n");
        return;
    }

    uint32_t failures = bus_err_nack + bus_err_timeout + bus_err_other;
    uint32_t per_mille = (bus_txn != 0u)
                         ? (uint32_t)(((uint64_t)failures * 1000u) / bus_txn)
                         : 0u;

    printf("txn %u, slave frames %u, failures %u (%u.%u%%)\n",
           (unsigned)bus_txn, (unsigned)bus_health_frames, (unsigned)failures,
           (unsigned)(per_mille / 10u), (unsigned)(per_mille % 10u));
    printf("  missed %u, truncated %u, buserr %u, flags 0x%02X%s%s%s%s%s\n",
           (unsigned)bus_health_missed, (unsigned)bus_health_truncated,
           bus_health_err_bus, bus_health_flags,
           (bus_health_flags & HEALTH_BUSERR) ? " buserr" : "",
           (bus_health_flags & HEALTH_ARLOST) ? " arlost" : "",
           (bus_health_flags & HEALTH_OUF) ? " ouf" : "",
           (bus_health_flags & HEALTH_SDA_STUCK) ? " sda-stuck" : "",
           (bus_health_flags & HEALTH_RECOVERED) ? " recovered" : "");
    printf("verdict: %s\n", health_verdict());

    {
        uint8_t st[2] = { 0, 0 };

        if (bus_read_reg(REG_STALL, st, sizeof(st)) == (int)sizeof(st)) {
            printf("manager: max stall %u us in state %u\n", st[0], st[1]);
        }
    }
}

static void cmd_watch(const char *arg)
{
    const char *what = ((arg != NULL) && (arg[0] != '\0')) ? arg : "status";

    printf("watch %s: any key stops\n", what);
    for (;;) {
        if (getchar_timeout_us(0) >= 0) {
            break;
        }

        if (strcmp(what, "temp") == 0) {
            uint8_t map[256];

            if (map_read(map)) {
                printf("cycle %u:", map_cycle(map));
                int ds = map[REG_DS18B20];

                for (int i = 0; (i < ds) && (i < (int)DEV_MAX); i++) {
                    uint16_t raw = (uint16_t)(map[REG_TEMP + 2 * i] |
                                              (map[REG_TEMP + 2 * i + 1] << 8));

                    printf(" ");
                    print_temp(raw);
                }
                printf("\n");
            } else {
                printf("read failed\n");
            }
        } else if (strcmp(what, "cycle") == 0) {
            uint8_t raw[2];

            if (bus_read_reg(REG_CYCLE_L, raw, 2u) == 2) {
                printf("cycle %u\n", (unsigned)(raw[0] | (raw[1] << 8)));
            } else {
                printf("read failed\n");
            }
        } else {
            print_status();
        }
        sleep_ms(1000u);
    }
    printf("\n[watch stopped]\n");
}

static void cmd_adc(void)
{
    if (!adc_ready) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        adc_gpio_init(29);           /* Pico VSYS/3 on ADC3 */
        adc_ready = true;
    }

    adc_select_input(3);
    uint32_t raw_v = adc_read();
    uint32_t mv = raw_v * 3u * 3300u / 4096u;

    adc_select_input(4);
    uint32_t raw_t = adc_read();
    int32_t milli_c = 27000 - (int32_t)(((int64_t)raw_t * 3300 * 1000 /
                                         4096 - 706000) / 1721);

    printf("VSYS %u mV, chip ", mv);
    if (milli_c < 0) {
        printf("-%d.%03d", (int)(-milli_c / 1000), (int)(-milli_c % 1000));
    } else {
        printf("%d.%03d", (int)(milli_c / 1000), (int)(milli_c % 1000));
    }
    printf(" C\n");
}

static const char *const seed_commands[] = {
    "bootsel", "adc", "watch temp", "bench", "sample", "dump", "roms",
    "eeprom", "scratch 0", "select 0", "cmd 1", "reg 0x00", "probe 0x42",
    "recover", "health", "health clear", "fuzz", "fuzz 500 0xC0FFEE",
    "stress 1000", "scan", "ping",
    "speed auto", "speed 400", "speed 100", "rescan",
    "info", "results", "run soak 60", "run full", "run quick", "help",
    "?", "temp"
};

static void seed_history(lineedit_t *le)
{
    lineedit_clear_history(le);
    for (unsigned i = 0; i < sizeof(seed_commands) / sizeof(seed_commands[0]);
         i++) {
        lineedit_add_history(le, seed_commands[i]);
    }
}

static void cmd_history(const char *arg)
{
    if ((arg != NULL) && (strcmp(arg, "clear") == 0)) {
        seed_history(&editor);
        printf("history reset to %d commands\n",
               lineedit_history_count(&editor));
        return;
    }

    for (int i = 0; i < lineedit_history_count(&editor); i++) {
        printf("%2d  %s\n", i + 1, lineedit_history_at(&editor, i));
    }
}

typedef struct {
    const char *name;
    const char *usage;
    const char *desc;
} cmd_desc_t;

static const cmd_desc_t command_table[] = {
    { "?", "?", "command list" },
    { "help", "help [cmd]", "command list or detail" },
    { "run", "run quick|full|soak N", "run the test suites" },
    { "results", "results", "print the last results" },
    { "status", "status", "uptime, link, target, snapshot" },
    { "info", "info", "version, clocks, pins, history size" },
    { "ping", "ping", "probe 0x42" },
    { "scan", "scan", "probe 0x08..0x77" },
    { "speed", "speed 100|400|auto", "set link and slave timing" },
    { "rescan", "rescan", "queue a ROM search" },
    { "temp", "temp [n]", "temperatures in C" },
    { "roms", "roms", "ROM table with family and CRC" },
    { "scratch", "scratch [n]", "decode a DS18B20 scratchpad" },
    { "eeprom", "eeprom", "DS2431 bytes and status" },
    { "dump", "dump [start] [len]", "hex dump of the register map" },
    { "reg", "reg <addr>", "read one register" },
    { "cmd", "cmd <n>", "write REG_CMD" },
    { "select", "select <n>", "write REG_SELECTED" },
    { "probe", "probe <addr>", "probe one I2C address" },
    { "recover", "recover", "nine clocks and STOP on the I2C bus" },
    { "health", "health [clear]", "bus health: transactions, losses, flags" },
    { "fuzz", "fuzz [ops] [seed]", "inject faults on both sides, seeded" },
    { "fault", "fault <code>", "arm one REG_FAULT code (1..5)" },
    { "sample", "sample [bytes]", "measure SCL with PIO" },
    { "bench", "bench", "100 vs 400 kHz read time" },
    { "watch", "watch temp|status|cycle", "1 Hz display until a key" },
    { "stress", "stress [n]", "n map reads at the current link speed" },
    { "repeat", "repeat <reg> <len> <n>", "n reads of one register block, with underrun counters" },
    { "diag", "diag", "underrun forensics, one register per transaction" },
    { "couple", "couple [n]", "same, with a strong 1-Wire load in every frame" },
    { "stall", "stall [n]", "same, with a 200 us CPU stall at byte 64" },
    { "rawspeed", "rawspeed <hz>", "set the master clock directly (calibration)" },
    { "adc", "adc", "VSYS and chip temperature" },
    { "clear", "clear", "clear the terminal" },
    { "history", "history [clear]", "show or reset command history" },
    { "abort", "abort", "stop the running suite" },
    { "bootsel", "bootsel", "reboot into the UF2 bootloader" },
};

static void cmd_help(const char *arg)
{
    if ((arg == NULL) || (arg[0] == '\0')) {
        for (unsigned i = 0; i < sizeof(command_table) / sizeof(command_table[0]);
             i++) {
            printf("  %-10s %s\n", command_table[i].name,
                   command_table[i].desc);
        }
        return;
    }

    for (unsigned i = 0; i < sizeof(command_table) / sizeof(command_table[0]);
         i++) {
        if (strcmp(command_table[i].name, arg) == 0) {
            printf("%s\n  %s\n", command_table[i].usage,
                   command_table[i].desc);
            return;
        }
    }
    printf("no help for %s\n", arg);
}

static int run_all(bool stress)
{
    tests_reset();
    running = true;
    fail_code = 0;
    led_next_us = 0u;

    suite_bus();
    if (!g_abort) {
        suite_map();
    }
    if (!g_abort) {
        suite_ow();
    }
    if (!g_abort && stress) {
        suite_stress();
    }
    if (!g_abort) {
        suite_fault();
    }

    running = false;

    int fail = count_failures();

    printf("\nSUMMARY: %d/%d passed, %d failed%s\n", g_result_count - fail,
           g_result_count, fail, g_abort ? " (aborted)" : "");
    if (fail == 0 && !g_abort) {
        printf("RESULT: PASS\n\n");
        fail_code = 0;
    } else {
        int first = first_failure_suite();

        printf("RESULT: FAIL first suite %d\n\n", first);
        fail_code = first != 0 ? first : 1;
    }
    return fail;
}

static void split_command(char *line, char **cmd, char **arg)
{
    char *p = line;

    while ((*p == ' ') || (*p == '\t')) {
        p++;
    }
    *cmd = p;
    while ((*p != '\0') && (*p != ' ') && (*p != '\t')) {
        p++;
    }
    if (*p != '\0') {
        *p++ = '\0';
        while ((*p == ' ') || (*p == '\t')) {
            p++;
        }
    }
    *arg = p;
}

static void handle_line(char *line)
{
    char *cmd;
    char *arg;

    split_command(line, &cmd, &arg);

    if ((strcmp(cmd, "help") == 0) || (strcmp(cmd, "?") == 0)) {
        cmd_help(arg);
        return;
    }
    if (strcmp(cmd, "info") == 0) {
        print_info();
        return;
    }
    if (strcmp(cmd, "status") == 0) {
        print_status();
        return;
    }
    if (strcmp(cmd, "ping") == 0) {
        printf("0x%02X: %s\n", BRIDGE_ADDR,
               bus_probe(BRIDGE_ADDR) ? "ack" : "nack");
        return;
    }
    if (strcmp(cmd, "scan") == 0) {
        print_scan();
        return;
    }
    if (strcmp(cmd, "rescan") == 0) {
        printf(send_cmd(CMD_RESCAN) ? "rescan queued\n" : "rescan failed\n");
        return;
    }
    if (strcmp(cmd, "temp") == 0) {
        cmd_temp(arg);
        return;
    }
    if (strcmp(cmd, "roms") == 0) {
        cmd_roms();
        return;
    }
    if (strcmp(cmd, "scratch") == 0) {
        cmd_scratch(arg);
        return;
    }
    if (strcmp(cmd, "eeprom") == 0) {
        cmd_eeprom();
        return;
    }
    if (strcmp(cmd, "dump") == 0) {
        cmd_dump(arg);
        return;
    }
    if (strcmp(cmd, "reg") == 0) {
        cmd_reg(arg);
        return;
    }
    if (strcmp(cmd, "cmd") == 0) {
        cmd_send_cmd(arg);
        return;
    }
    if (strcmp(cmd, "select") == 0) {
        cmd_select(arg);
        return;
    }
    if (strcmp(cmd, "probe") == 0) {
        cmd_probe(arg);
        return;
    }
    if (strcmp(cmd, "recover") == 0) {
        bus_recover();
        printf("bus recovery done\n");
        return;
    }
    if (strcmp(cmd, "health") == 0) {
        cmd_health(arg);
        return;
    }
    if (strcmp(cmd, "fuzz") == 0) {
        cmd_fuzz(arg);
        return;
    }
    if (strcmp(cmd, "fault") == 0) {
        cmd_fault(arg);
        return;
    }
    if (strcmp(cmd, "sample") == 0) {
        cmd_sample(arg);
        return;
    }
    if (strcmp(cmd, "bench") == 0) {
        cmd_bench();
        return;
    }
    if (strcmp(cmd, "speed") == 0) {
        int khz = atoi(arg);

        if ((khz >= 50) && (khz <= 1000)) {
            uint8_t slave = (khz > 100) ? 1u : 0u;
            uint32_t request = (khz == 400) ? BUS_SPEED_FAST
                             : (khz == 100) ? BUS_SPEED_100K
                                            : (uint32_t)khz * 1000u;

            bus_set_speed(BUS_SPEED_100K);
            write_speed(slave);
            sleep_ms(30u);
            bus_set_speed(request);

            printf("link %d kHz\n", (khz == 400) ? 400 : khz);
        } else if (strcmp(arg, "auto") == 0) {
            cmd_speed_auto();
        } else {
            printf("usage: speed 50..1000 | auto\n");
        }
        return;
    }
    if (strcmp(cmd, "watch") == 0) {
        cmd_watch(arg);
        return;
    }
    if (strcmp(cmd, "stress") == 0) {
        uint32_t count = (arg[0] != '\0') ? (uint32_t)strtoul(arg, NULL, 0)
                                          : 1000u;

        stress_run(count == 0u ? 1000u : count);
        return;
    }
    if (strcmp(cmd, "diag") == 0) {
        diag_run();
        return;
    }
    if (strcmp(cmd, "repeat") == 0) {
        char *end = NULL;
        unsigned long reg = strtoul(arg, &end, 0);
        unsigned long len = (end != NULL) ? strtoul(end, &end, 0) : 0ul;
        unsigned long n = (end != NULL) ? strtoul(end, &end, 0) : 0ul;

        if ((end == NULL) || (*end != '\0') || (reg > 255ul)) {
            printf("usage: repeat <reg> <len> <count>\n");
        } else {
            repeat_run((uint8_t)reg, (uint32_t)len, (uint32_t)n);
        }
        return;
    }
    if (strcmp(cmd, "couple") == 0) {
        uint32_t count = (arg[0] != '\0') ? (uint32_t)strtoul(arg, NULL, 0)
                                          : 1000u;

        couple_run(count == 0u ? 1000u : count);
        return;
    }
    if (strcmp(cmd, "stall") == 0) {
        uint32_t count = (arg[0] != '\0') ? (uint32_t)strtoul(arg, NULL, 0)
                                          : 1000u;

        stall_run(count == 0u ? 1000u : count);
        return;
    }
    if (strcmp(cmd, "rawspeed") == 0) {
        uint32_t hz = (uint32_t)strtoul(arg, NULL, 0);

        if (hz >= 50000u) {
            bus_set_speed(hz);
            printf("raw %u Hz\n", (unsigned)hz);
        } else {
            printf("usage: rawspeed <hz>\n");
        }
        return;
    }
    if (strcmp(cmd, "adc") == 0) {
        cmd_adc();
        return;
    }
    if (strcmp(cmd, "clear") == 0) {
        printf("\x1b[2J\x1b[H");
        return;
    }
    if (strcmp(cmd, "history") == 0) {
        cmd_history(arg);
        return;
    }
    if (strcmp(cmd, "run") == 0) {
        if (strcmp(arg, "quick") == 0) {
            run_all(false);
        } else if (strcmp(arg, "full") == 0) {
            run_all(true);
        } else if (strncmp(arg, "soak ", 5) == 0) {
            uint32_t seconds = (uint32_t)strtoul(&arg[5], NULL, 10);

            if (seconds == 0u) {
                printf("usage: run soak N\n");
            } else {
                g_soak_seconds = seconds;
                run_all(true);
            }
        } else {
            printf("usage: run quick | full | soak N\n");
        }
        return;
    }
    if (strcmp(cmd, "results") == 0) {
        print_results();
        return;
    }
    if (strcmp(cmd, "abort") == 0) {
        g_abort = true;
        printf("abort requested\n");
        return;
    }
    if (strcmp(cmd, "bootsel") == 0) {
        printf("rebooting to BOOTSEL\n");
        sleep_ms(50u);
        reset_usb_boot(0, 0);
        return;
    }
    if (cmd[0] != '\0') {
        printf("unknown command: %s (try ?)\n", cmd);
    }
}

int main(void)
{
    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    bus_init();
    bus_health_reset();
    scl_probe_init(SAMPLER_PIN);

    for (int i = 0; i < 30 && !stdio_usb_connected(); i++) {
        sleep_ms(100u);
    }

    printf("\nAT32F421 I2C bridge tester (RP2040)\n");
    printf("SDA GP4, SCL GP5, target 0x%02X, external 2.2k pull-ups\n\n",
           BRIDGE_ADDR);

    if (!bus_probe(BRIDGE_ADDR)) {
        printf("TARGET ABSENT: no ACK at 0x%02X\n", BRIDGE_ADDR);
        printf("RESULT: FAIL (no target)\n");
        fail_code = 1;
    } else {
        run_all(true);
    }

    lineedit_init(&editor, "> ");
    seed_history(&editor);
    printf("ready: ? for commands\n");
    lineedit_redraw(&editor);

    for (;;) {
        int c;

        led_poll();
        c = getchar_timeout_us(0);
        if (c >= 0) {
            const char *line = lineedit_feed(&editor, c);

            if (line != NULL) {
                handle_line((char *)line);
                lineedit_redraw(&editor);
            }
        }
        tight_loop_contents();
    }
}


