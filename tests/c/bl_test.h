/* Minimal test harness for the BruceLink core. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int bl_tests_run;
extern int bl_tests_failed;

#define CHECK(cond)                                                      \
    do {                                                                 \
        bl_tests_run++;                                                  \
        if(!(cond)) {                                                    \
            bl_tests_failed++;                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                \
    } while(0)

#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        bl_tests_run++;                                                          \
        long _va = (long)(a), _vb = (long)(b);                                   \
        if(_va != _vb) {                                                         \
            bl_tests_failed++;                                                   \
            fprintf(                                                             \
                stderr, "FAIL %s:%d: %s=%ld != %s=%ld\n", __FILE__, __LINE__,    \
                #a, _va, #b, _vb);                                               \
        }                                                                        \
    } while(0)

static inline int bl_hex2bin(const char* hex, uint8_t* out, size_t out_cap) {
    size_t n = strlen(hex) / 2;
    if(n > out_cap) return -1;
    for(size_t i = 0; i < n; i++) {
        unsigned v;
        if(sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)n;
}

#define RUN(fn)                               \
    do {                                      \
        int before = bl_tests_failed;         \
        fn();                                  \
        printf(                                \
            "  %-28s %s\n", #fn,               \
            (bl_tests_failed == before) ? "ok" : "FAIL"); \
    } while(0)
