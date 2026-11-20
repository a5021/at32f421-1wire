#ifndef TESTER_TESTS_H
#define TESTER_TESTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_RESULTS     64

typedef struct {
    char        id[5];
    const char *name;
    bool        ok;
    char        detail[112];
} test_result_t;

extern test_result_t g_results[MAX_RESULTS];
extern int           g_result_count;
extern bool          g_abort;
extern uint32_t      g_soak_seconds;

void tests_reset(void);
void t_start(const char *id, const char *name);
void t_pass(const char *fmt, ...);
void t_fail(const char *fmt, ...);

/* LED / abort input, implemented in main.c. */
void service(void);

/* Shared snapshot checks. */
bool map_validate(const uint8_t *map, char *why, size_t why_len);
int  map_stored_devices(const uint8_t *map);
bool map_rom_equal(const uint8_t *a, const uint8_t *b);

#endif /* TESTER_TESTS_H */
