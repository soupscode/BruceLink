/*
 * BruceLink shared core — COBS framing.
 * SPDX-License-Identifier: MIT
 */
#include "bl_cobs.h"

#include <string.h>

int bl_cobs_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if(in == NULL || out == NULL) return BL_E_ARG;
    if(out_cap < BL_COBS_MAX_ENCODED(in_len)) return BL_E_OVERFLOW;

    size_t read = 0;
    size_t write = 0;
    size_t code_pos = write++; /* reserve first code byte */
    uint8_t code = 1;

    while(read < in_len) {
        uint8_t b = in[read++];
        if(b != 0) {
            out[write++] = b;
            code++;
            if(code != 0xFF) continue;
        }
        out[code_pos] = code;
        code_pos = write++;
        code = 1;
    }
    out[code_pos] = code;
    out[write++] = 0x00; /* delimiter */
    return (int)write;
}

int bl_cobs_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if(in == NULL || out == NULL) return BL_E_ARG;
    size_t read = 0;
    size_t write = 0;
    while(read < in_len) {
        uint8_t code = in[read++];
        if(code == 0) return BL_E_FORMAT; /* delimiter inside frame */
        if(read + (size_t)(code - 1) > in_len) return BL_E_FORMAT;
        for(uint8_t i = 1; i < code; i++) {
            if(write >= out_cap) return BL_E_OVERFLOW;
            out[write++] = in[read++];
        }
        if(code != 0xFF && read < in_len) {
            if(write >= out_cap) return BL_E_OVERFLOW;
            out[write++] = 0x00;
        }
    }
    return (int)write;
}

void bl_cobs_stream_init(bl_cobs_stream_t* s) {
    if(s == NULL) return;
    s->len = 0;
    s->overflow = false;
}

int bl_cobs_stream_push(
    bl_cobs_stream_t* s,
    uint8_t byte,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(s == NULL || out == NULL || out_len == NULL) return BL_E_ARG;
    if(byte != 0x00) {
        if(s->len >= sizeof(s->buf)) {
            s->overflow = true; /* keep consuming until delimiter */
        } else {
            s->buf[s->len++] = byte;
        }
        return BL_E_SHORT;
    }
    /* delimiter: frame complete */
    size_t n = s->len;
    bool of = s->overflow;
    s->len = 0;
    s->overflow = false;
    if(of) return BL_E_OVERFLOW;
    if(n == 0) return BL_E_SHORT; /* empty frame / keepalive */
    int rc = bl_cobs_decode(s->buf, n, out, out_cap);
    if(rc < 0) return rc;
    *out_len = (size_t)rc;
    return BL_OK;
}
