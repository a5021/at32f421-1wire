#ifndef TEST_H
#define TEST_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern int tf_checks;
extern int tf_failures;

void tf_begin(const char *name);
void tf_end(void);
void tf_fail_expr(const char *file, int line, const char *expr);
void tf_fail_cmp(const char *file, int line, const char *what,
                 long long expected, long long actual);
void tf_fail_mem(const char *file, int line, const char *what,
                 const void *expected, const void *actual, int len);
int  tf_summary(void);

#define TF_CHECK(cond)                                                   \
    do {                                                                 \
        tf_checks++;                                                     \
        if (!(cond)) {                                                   \
            tf_fail_expr(__FILE__, __LINE__, #cond);                     \
        }                                                                \
    } while (0)

#define TF_CHECK_EQ(expected, actual)                                    \
    do {                                                                 \
        tf_checks++;                                                     \
        long long e_ = (long long)(expected);                            \
        long long a_ = (long long)(actual);                              \
        if (e_ != a_) {                                                  \
            tf_fail_cmp(__FILE__, __LINE__, #actual, e_, a_);            \
        }                                                                \
    } while (0)

#define TF_CHECK_MEM(expected, actual, len)                              \
    do {                                                                 \
        tf_checks++;                                                     \
        if (memcmp((expected), (actual), (size_t)(len)) != 0) {          \
            tf_fail_mem(__FILE__, __LINE__, #actual,                     \
                        (expected), (actual), (int)(len));               \
        }                                                                \
    } while (0)

#define TF_CHECK_NEAR(expected, actual, eps)                             \
    do {                                                                 \
        tf_checks++;                                                     \
        double e_ = (double)(expected);                                  \
        double a_ = (double)(actual);                                    \
        double d_ = (e_ > a_) ? (e_ - a_) : (a_ - e_);                   \
        if (d_ > (double)(eps)) {                                        \
            tf_fail_cmp(__FILE__, __LINE__, #actual,                     \
                        (long long)(e_ * 10000.0),                       \
                        (long long)(a_ * 10000.0));                      \
        }                                                                \
    } while (0)

typedef struct {
    const char *name;
    void (*run)(void);
} tf_suite_t;

#define TF_RUN(suite)                                                    \
    do {                                                                 \
        tf_begin(#suite);                                                \
        suite();                                                         \
        tf_end();                                                        \
    } while (0)

#endif /* TEST_H */
