/*
 * BruceLink shared core — fragmentation and reassembly.
 *
 * A frame (up to BL_MAX_MSG bytes) is split into chunks that fit a
 * transport's usable payload (e.g. a BLE GATT write, ~244 bytes). Each
 * chunk carries a 2-byte fragment header:
 *   [0] flags: bit0 FIRST, bit1 LAST
 *   [1] index: 0..255 wrapping, for gap detection within one frame
 * BLE preserves message boundaries per write/notify, so each chunk is
 * delivered whole; this layer only handles splitting and ordered
 * reassembly, not byte-stream resync (see bl_cobs for serial).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_FRAG_HDR 2u
#define BL_FRAG_FIRST 0x01u
#define BL_FRAG_LAST 0x02u

/* Smallest transport MTU we support must carry header + 1 byte. */
#define BL_MIN_CHUNK (BL_FRAG_HDR + 1u)

typedef struct {
    uint8_t buf[BL_MAX_MSG];
    size_t len;
    uint8_t next_index;
    bool in_progress;
} bl_reasm_t;

void bl_reasm_init(bl_reasm_t* r);

/*
 * Emit the chunk at fragment position `frag_i` for `msg` (msg_len bytes)
 * given a transport chunk capacity `chunk_cap` (>= BL_MIN_CHUNK).
 * Writes to out and sets *out_len. Returns 1 if this was the last
 * chunk, 0 if more remain, or a negative bl_result_t on error.
 * Iterate frag_i = 0,1,2,... until it returns 1.
 */
int bl_frag_chunk(
    const uint8_t* msg,
    size_t msg_len,
    size_t chunk_cap,
    unsigned frag_i,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);

/* Number of chunks a message of msg_len needs at chunk_cap. */
size_t bl_frag_count(size_t msg_len, size_t chunk_cap);

/*
 * Feed one received chunk. On the chunk completing a frame, returns
 * BL_OK and sets frame/frame_len to the reassembled frame (pointing
 * into r->buf). While more chunks are expected it returns BL_OK with
 * frame_len==0. On protocol error (unexpected
 * index, overflow, missing FIRST) resets and returns BL_E_SEQ/
 * BL_E_OVERFLOW/BL_E_FORMAT.
 */
int bl_reasm_push(
    bl_reasm_t* r,
    const uint8_t* chunk,
    size_t chunk_len,
    const uint8_t** frame,
    size_t* frame_len);

#ifdef __cplusplus
}
#endif
