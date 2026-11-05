#ifndef AURORA_TEST_HARNESS_H
#define AURORA_TEST_HARNESS_H
#include <stdio.h>
#include <stdint.h>
#include <string.h>

extern int t_failures, t_checks;
extern const char *t_name;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        t_checks++;                                                                        \
        if (!(cond)) {                                                                     \
            t_failures++;                                                                  \
            fprintf(stderr, "  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, t_name, #cond); \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                               \
    do {                                                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                                          \
        t_checks++;                                                                                  \
        if (_a != _b) {                                                                              \
            t_failures++;                                                                            \
            fprintf(stderr, "  FAIL %s:%d [%s] %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, t_name, \
                    #a, #b, _a, _b);                                                                 \
        }                                                                                            \
    } while (0)

#define CHECK_STR(a, b)                                                                              \
    do {                                                                                             \
        t_checks++;                                                                                  \
        if (strcmp((a), (b)) != 0) {                                                                 \
            t_failures++;                                                                            \
            fprintf(stderr, "  FAIL %s:%d [%s] \"%s\" != \"%s\"\n", __FILE__, __LINE__, t_name, (a), (b)); \
        }                                                                                            \
    } while (0)

#define TEST(name) static void name(void)
#define RUN(name)        \
    do {                 \
        t_name = #name;  \
        name();          \
    } while (0)

void run_tests(void);
extern const char *t_suite; /* defined by each test file */

#endif
