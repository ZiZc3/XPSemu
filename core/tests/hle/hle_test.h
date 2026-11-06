/* Minimal assertion harness for hakuX HLE host tests. */
#ifndef TESTS_HLE_HLE_TEST_H
#define TESTS_HLE_HLE_TEST_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int ht_failures;

#define HT_CHECK(c) do { \
    if (!(c)) { \
        fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
        ht_failures++; \
    } \
} while (0)

#define HT_EQ_U32(a, b) do { \
    uint32_t _a = (uint32_t)(a), _b = (uint32_t)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: %s == 0x%08x, expected 0x%08x\n", \
                __FILE__, __LINE__, #a, _a, _b); \
        ht_failures++; \
    } \
} while (0)

#define HT_EQ_INT(a, b) do { \
    long _a = (long)(a), _b = (long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "%s:%d: %s == %ld, expected %ld\n", \
                __FILE__, __LINE__, #a, _a, _b); \
        ht_failures++; \
    } \
} while (0)

#define HT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", \
                __FILE__, __LINE__, #a, _a, _b); \
        ht_failures++; \
    } \
} while (0)

#define HT_RUN(fn) do { \
    int _before = ht_failures; \
    fn(); \
    printf("%s %s\n", _before == ht_failures ? "PASS" : "FAIL", #fn); \
} while (0)

#define HT_DONE() return ht_failures ? 1 : 0

#endif
