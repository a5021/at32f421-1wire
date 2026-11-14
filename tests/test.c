#include "test.h"

int tf_checks;
int tf_failures;

static const char *tf_suite_name;
static int tf_suite_failures;

void tf_begin(const char *name)
{
    tf_suite_name = name;
    tf_suite_failures = 0;
    printf("[ RUN      ] %s\n", name);
}

void tf_end(void)
{
    if (tf_suite_failures == 0) {
        printf("[       OK ] %s\n", tf_suite_name);
    } else {
        printf("[  FAILED  ] %s (%d failure(s))\n", tf_suite_name,
               tf_suite_failures);
    }
}

void tf_fail_expr(const char *file, int line, const char *expr)
{
    tf_failures++;
    tf_suite_failures++;
    printf("[  FAILED  ] %s:%d: check failed: %s\n", file, line, expr);
}

void tf_fail_cmp(const char *file, int line, const char *what,
                 long long expected, long long actual)
{
    tf_failures++;
    tf_suite_failures++;
    printf("[  FAILED  ] %s:%d: %s expected %lld, got %lld\n",
           file, line, what, expected, actual);
}

void tf_fail_mem(const char *file, int line, const char *what,
                 const void *expected, const void *actual, int len)
{
    const uint8_t *e = (const uint8_t *)expected;
    const uint8_t *a = (const uint8_t *)actual;
    int i;

    tf_failures++;
    tf_suite_failures++;
    printf("[  FAILED  ] %s:%d: %s differs (len %d)\n", file, line, what, len);
    for (i = 0; i < len; i++) {
        if (e[i] != a[i]) {
            printf("              first at %d: expected 0x%02X, got 0x%02X\n",
                   i, e[i], a[i]);
            break;
        }
    }
}

int tf_summary(void)
{
    printf("\n%d checks, %d failure(s)\n", tf_checks, tf_failures);
    return (tf_failures == 0) ? 0 : 1;
}
