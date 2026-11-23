/* Bounded text builder — regression tests for the snprintf length
 * bookkeeping defect class (PR #1 review: Wi-Fi scan output length).
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_textbuf.h"
#include "bl_test.h"

#include <limits.h>

static void t_tb_basic(void) {
    char b[32];
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_str(&tb, "ab"));
    CHECK(bl_tb_int(&tb, -42));
    CHECK(bl_tb_uint(&tb, 7));
    CHECK(bl_tb_hex16(&tb, 0x0a40));
    CHECK(strcmp(b, "ab-4270a40") == 0);
    CHECK_EQ_INT(tb.len, strlen(b));
    CHECK(!tb.truncated);
}

static void t_tb_truncation_is_atomic_and_latched(void) {
    char b[8]; /* room for 7 chars + NUL */
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_str(&tb, "12345"));
    /* "678" would need len 8 -> does not fit: nothing appended */
    CHECK(!bl_tb_str(&tb, "678"));
    CHECK(tb.truncated);
    CHECK_EQ_INT(tb.len, 5);
    CHECK(strcmp(b, "12345") == 0);
    /* later (even short) appends are refused: output stays a clean prefix */
    CHECK(!bl_tb_str(&tb, "x"));
    CHECK_EQ_INT(tb.len, 5);
    CHECK(strcmp(b, "12345") == 0);
}

static void t_tb_exact_fit(void) {
    char b[4];
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_str(&tb, "abc")); /* 3 chars + NUL == cap */
    CHECK_EQ_INT(tb.len, 3);
    CHECK(!bl_tb_str(&tb, "d"));
    CHECK(strcmp(b, "abc") == 0);
}

static void t_tb_zero_and_null_capacity(void) {
    char b[1] = {'Z'};
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, 0); /* zero capacity: never writes */
    CHECK(!bl_tb_str(&tb, ""));
    CHECK(!bl_tb_int(&tb, 1));
    CHECK_EQ_INT(tb.len, 0);
    CHECK(b[0] == 'Z'); /* untouched */
    bl_tb_init(&tb, NULL, 100); /* NULL buffer is treated as zero capacity */
    CHECK(!bl_tb_str(&tb, "x"));
    CHECK_EQ_INT(tb.len, 0);
    /* capacity 1 holds only the terminator */
    char one[1];
    bl_tb_init(&tb, one, 1);
    CHECK(one[0] == '\0');
    CHECK(!bl_tb_str(&tb, "a"));
    CHECK_EQ_INT(tb.len, 0);
}

static void t_tb_long_input_tiny_buffer(void) {
    char longs[300];
    memset(longs, 'S', sizeof(longs) - 1);
    longs[sizeof(longs) - 1] = '\0';
    char b[16];
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(!bl_tb_str(&tb, longs));
    CHECK_EQ_INT(tb.len, 0);
    CHECK(b[0] == '\0');
    /* sanitized respects max_chars even with a long source */
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_sanitized(&tb, longs, 10));
    CHECK_EQ_INT(tb.len, 10);
}

static void t_tb_sanitized_blocks_line_injection(void) {
    char b[64];
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_sanitized(&tb, "evil\nFAKE LINE\r\x01\x7f", 32));
    CHECK(strchr(b, '\n') == NULL);
    CHECK(strchr(b, '\r') == NULL);
    CHECK(strcmp(b, "evil?FAKE LINE???") == 0);
    /* non-ASCII (UTF-8) bytes are preserved */
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_sanitized(&tb, "caf\xc3\xa9", 32));
    CHECK(strcmp(b, "caf\xc3\xa9") == 0);
}

static void t_tb_int_extremes(void) {
    char b[32];
    bl_textbuf_t tb;
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_int(&tb, LONG_MIN));
    char expect[32];
    snprintf(expect, sizeof(expect), "%ld", LONG_MIN);
    CHECK(strcmp(b, expect) == 0);
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_uint(&tb, ULONG_MAX));
    snprintf(expect, sizeof(expect), "%lu", ULONG_MAX);
    CHECK(strcmp(b, expect) == 0);
    bl_tb_init(&tb, b, sizeof(b));
    CHECK(bl_tb_int(&tb, 0));
    CHECK(strcmp(b, "0") == 0);
}

void run_textbuf_tests(void) {
    RUN(t_tb_basic);
    RUN(t_tb_truncation_is_atomic_and_latched);
    RUN(t_tb_exact_fit);
    RUN(t_tb_zero_and_null_capacity);
    RUN(t_tb_long_input_tiny_buffer);
    RUN(t_tb_sanitized_blocks_line_injection);
    RUN(t_tb_int_extremes);
}
