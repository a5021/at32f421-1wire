#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

#include "i2c_util.h"
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

static bool     running;
static int      fail_code;
static uint64_t led_next_us;
static bool     led_state;
static int      fail_step;
static uint64_t fail_next_us;

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
        if ((c == 3) || (c == 'a') || (c == 'A')) {
            if (!g_abort) {
                printf("\n[abort requested]\n");
            }
            g_abort = true;
        }
    }
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
}

static void print_status(void)
{
    uint8_t hdr[16];
    uint32_t up_s = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000u);
    bool ack = bus_probe(BRIDGE_ADDR);

    printf("uptime %u s, link %u kHz\n", up_s,
           (unsigned)(bus_speed() / 1000u));
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

    running = false;

    int fail = 0;

    for (int i = 0; i < g_result_count; i++) {
        if (!g_results[i].ok) {
            fail++;
        }
    }

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

static void handle_line(char *line)
{
    if ((strcmp(line, "help") == 0) || (strcmp(line, "?") == 0)) {
        printf("run quick | run full | run soak N | results | abort\n");
        printf("info | status | ping | scan | speed 100|400 | rescan | bootsel\n");
        return;
    }
    if (strcmp(line, "info") == 0) {
        print_info();
        return;
    }
    if (strcmp(line, "status") == 0) {
        print_status();
        return;
    }
    if (strcmp(line, "ping") == 0) {
        printf("0x%02X: %s\n", BRIDGE_ADDR,
               bus_probe(BRIDGE_ADDR) ? "ack" : "nack");
        return;
    }
    if (strcmp(line, "scan") == 0) {
        print_scan();
        return;
    }
    if (strcmp(line, "rescan") == 0) {
        if (send_cmd(CMD_RESCAN)) {
            printf("rescan queued\n");
        } else {
            printf("rescan failed\n");
        }
        return;
    }
    if (strncmp(line, "speed ", 6) == 0) {
        int khz = atoi(&line[6]);
        uint8_t readback = 0xEEu;

        if (khz == 100) {
            bus_set_speed(BUS_SPEED_100K);
            write_speed(0u);
            bus_read_reg(REG_SPEED, &readback, 1u);
            printf("link %u kHz, slave %u\n",
                   (unsigned)(bus_speed() / 1000u), readback);
        } else if (khz == 400) {
            bus_set_speed(BUS_SPEED_100K);
            write_speed(1u);
            sleep_ms(30u);
            bus_set_speed(BUS_SPEED_400K);
            bus_read_reg(REG_SPEED, &readback, 1u);
            printf("link %u kHz, slave %u\n",
                   (unsigned)(bus_speed() / 1000u), readback);
        } else {
            printf("usage: speed 100 | speed 400\n");
        }
        return;
    }
    if (strcmp(line, "run quick") == 0) {
        run_all(false);
        return;
    }
    if (strcmp(line, "run full") == 0) {
        run_all(true);
        return;
    }
    if (strncmp(line, "run soak ", 9) == 0) {
        uint32_t seconds = (uint32_t)strtoul(&line[9], NULL, 10);

        if (seconds == 0u) {
            printf("usage: run soak N (seconds)\n");
            return;
        }
        g_soak_seconds = seconds;
        run_all(true);
        return;
    }
    if (strcmp(line, "results") == 0) {
        print_results();
        return;
    }
    if (strcmp(line, "abort") == 0) {
        g_abort = true;
        printf("abort requested\n");
        return;
    }
    if (strcmp(line, "bootsel") == 0) {
        printf("rebooting to BOOTSEL\n");
        sleep_ms(50u);
        reset_usb_boot(0, 0);
        return;
    }
    if (line[0] != '\0') {
        printf("unknown command: %s (try ?)\n", line);
    }
}

int main(void)
{
    char line[64];
    int line_len = 0;

    stdio_init_all();
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    bus_init();
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

    printf("ready: ? for commands\n");
    printf("> ");

    for (;;) {
        int c;

        led_poll();
        while ((c = getchar_timeout_us(0)) >= 0) {
            if ((c == '\r') || (c == '\n')) {
                printf("\n");
                line[line_len] = '\0';
                handle_line(line);
                line_len = 0;
                printf("> ");
            } else if ((c == 8) || (c == 127)) {
                if (line_len > 0) {
                    line_len--;
                    printf("\b \b");
                }
            } else if ((c >= 32) && (c < 127) &&
                       (line_len < (int)sizeof(line) - 1)) {
                line[line_len++] = (char)c;
                putchar(c);
            }
        }
        tight_loop_contents();
    }
}
