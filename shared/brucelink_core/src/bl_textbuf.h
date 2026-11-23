/*
 * BruceLink shared core — bounded text builder.
 *
 * Builds human-readable response payloads without ever overrunning the
 * destination or mis-reporting its length (the defect class of summing
 * snprintf return values, which report the would-have-written length on
 * truncation). Typed appenders, no printf: vsnprintf is not exported to
 * Flipper FAPs, and this keeps every bound obvious.
 *
 * Semantics:
 *   - len < cap whenever cap > 0, and buf[len] == '\0' (always terminated).
 *   - Each append is all-or-nothing. The first append that does not fit
 *     latches `truncated`; later appends are refused, so the contents are
 *     always a clean prefix of what was intended.
 *   - cap == 0 (or buf == NULL) is legal: every append fails, len stays 0.
 *
 * bl_tb_sanitized() is for UNTRUSTED text (e.g. broadcast SSIDs): control
 * characters, including newlines, become '?', so peer-supplied bytes
 * cannot inject extra lines into line-oriented output.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* buf;
    size_t cap;
    size_t len; /* bytes of text, excluding the terminator */
    bool truncated;
} bl_textbuf_t;

void bl_tb_init(bl_textbuf_t* tb, char* buf, size_t cap);

/* Trusted text. */
bool bl_tb_str(bl_textbuf_t* tb, const char* s);
/* Untrusted text: at most max_chars source bytes; control chars -> '?'. */
bool bl_tb_sanitized(bl_textbuf_t* tb, const char* s, size_t max_chars);
/* Signed / unsigned decimal. */
bool bl_tb_int(bl_textbuf_t* tb, long v);
bool bl_tb_uint(bl_textbuf_t* tb, unsigned long v);
/* Exactly four lowercase hex digits. */
bool bl_tb_hex16(bl_textbuf_t* tb, uint16_t v);

#ifdef __cplusplus
}
#endif
