/*
 * BruceLink shared core — fragmentation and reassembly.
 * SPDX-License-Identifier: MIT
 */
#include "bl_reasm.h"

#include <string.h>

void bl_reasm_init(bl_reasm_t* r) {
    if(r == NULL) return;
    r->len = 0;
    r->next_index = 0;
    r->in_progress = false;
}

size_t bl_frag_count(size_t msg_len, size_t chunk_cap) {
    if(chunk_cap < BL_MIN_CHUNK) return 0;
    size_t data_per = chunk_cap - BL_FRAG_HDR;
    if(msg_len == 0) return 1;
    return (msg_len + data_per - 1) / data_per;
}

int bl_frag_chunk(
    const uint8_t* msg,
    size_t msg_len,
    size_t chunk_cap,
    unsigned frag_i,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(msg == NULL || out == NULL || out_len == NULL) return BL_E_ARG;
    if(chunk_cap < BL_MIN_CHUNK) return BL_E_ARG;
    if(msg_len > BL_MAX_MSG) return BL_E_TOO_LARGE;
    size_t data_per = chunk_cap - BL_FRAG_HDR;
    size_t total = bl_frag_count(msg_len, chunk_cap);
    if(frag_i >= total) return BL_E_ARG;

    size_t off = (size_t)frag_i * data_per;
    size_t take = msg_len - off;
    if(take > data_per) take = data_per;
    if(out_cap < BL_FRAG_HDR + take) return BL_E_OVERFLOW;

    uint8_t flags = 0;
    if(frag_i == 0) flags |= BL_FRAG_FIRST;
    if(frag_i + 1 == total) flags |= BL_FRAG_LAST;
    out[0] = flags;
    out[1] = (uint8_t)(frag_i & 0xFF);
    if(take) memcpy(out + BL_FRAG_HDR, msg + off, take);
    *out_len = BL_FRAG_HDR + take;
    return (frag_i + 1 == total) ? 1 : 0;
}

int bl_reasm_push(
    bl_reasm_t* r,
    const uint8_t* chunk,
    size_t chunk_len,
    const uint8_t** frame,
    size_t* frame_len) {
    if(r == NULL || chunk == NULL || frame == NULL || frame_len == NULL) return BL_E_ARG;
    *frame = NULL;
    *frame_len = 0;
    if(chunk_len < BL_FRAG_HDR) return BL_E_FORMAT;
    uint8_t flags = chunk[0];
    uint8_t index = chunk[1];
    const uint8_t* data = chunk + BL_FRAG_HDR;
    size_t dlen = chunk_len - BL_FRAG_HDR;

    if(flags & BL_FRAG_FIRST) {
        r->len = 0;
        r->next_index = 0;
        r->in_progress = true;
    }
    if(!r->in_progress) return BL_E_SEQ; /* missing FIRST */
    if(index != r->next_index) {
        bl_reasm_init(r);
        return BL_E_SEQ; /* out-of-order / lost fragment */
    }
    if(r->len + dlen > BL_MAX_MSG) {
        bl_reasm_init(r);
        return BL_E_OVERFLOW;
    }
    memcpy(r->buf + r->len, data, dlen);
    r->len += dlen;
    r->next_index++;

    if(flags & BL_FRAG_LAST) {
        *frame = r->buf;
        *frame_len = r->len;
        r->in_progress = false;
        return BL_OK;
    }
    return BL_OK;
}
