/*
 * BruceLink shared core — bounded text builder.
 * SPDX-License-Identifier: MIT
 */
#include "bl_textbuf.h"

#include <string.h>

void bl_tb_init(bl_textbuf_t* tb, char* buf, size_t cap) {
    if(tb == NULL) return;
    tb->buf = buf;
    tb->cap = (buf != NULL) ? cap : 0;
    tb->len = 0;
    tb->truncated = false;
    if(tb->cap > 0) tb->buf[0] = '\0';
}

/* Reserve n bytes (plus terminator) or latch truncation. */
static bool tb_reserve(bl_textbuf_t* tb, size_t n) {
    if(tb == NULL || tb->truncated) return false;
    if(tb->cap == 0 || n >= tb->cap - tb->len) {
        tb->truncated = true;
        return false;
    }
    return true;
}

static bool tb_put(bl_textbuf_t* tb, const char* s, size_t n) {
    if(!tb_reserve(tb, n)) return false;
    if(n) memcpy(tb->buf + tb->len, s, n);
    tb->len += n;
    tb->buf[tb->len] = '\0';
    return true;
}

bool bl_tb_str(bl_textbuf_t* tb, const char* s) {
    if(s == NULL) s = "";
    return tb_put(tb, s, strlen(s));
}

bool bl_tb_sanitized(bl_textbuf_t* tb, const char* s, size_t max_chars) {
    if(s == NULL) s = "";
    size_t n = 0;
    while(n < max_chars && s[n] != '\0') n++;
    if(!tb_reserve(tb, n)) return false;
    for(size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        tb->buf[tb->len + i] = (c < 0x20u || c == 0x7fu) ? '?' : (char)c;
    }
    tb->len += n;
    tb->buf[tb->len] = '\0';
    return true;
}

bool bl_tb_uint(bl_textbuf_t* tb, unsigned long v) {
    char tmp[24]; /* 2^64-1 has 20 digits */
    size_t i = sizeof(tmp);
    do {
        tmp[--i] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while(v != 0u && i > 0);
    return tb_put(tb, tmp + i, sizeof(tmp) - i);
}

bool bl_tb_int(bl_textbuf_t* tb, long v) {
    /* Magnitude via unsigned arithmetic: well-defined for LONG_MIN. */
    unsigned long mag = (v < 0) ? 0UL - (unsigned long)v : (unsigned long)v;
    char tmp[24];
    size_t i = sizeof(tmp);
    do {
        tmp[--i] = (char)('0' + (int)(mag % 10u));
        mag /= 10u;
    } while(mag != 0u && i > 1);
    if(v < 0) tmp[--i] = '-';
    return tb_put(tb, tmp + i, sizeof(tmp) - i);
}

bool bl_tb_hex16(bl_textbuf_t* tb, uint16_t v) {
    static const char digits[] = "0123456789abcdef";
    char tmp[4];
    tmp[0] = digits[(v >> 12) & 0xFu];
    tmp[1] = digits[(v >> 8) & 0xFu];
    tmp[2] = digits[(v >> 4) & 0xFu];
    tmp[3] = digits[v & 0xFu];
    return tb_put(tb, tmp, sizeof(tmp));
}
