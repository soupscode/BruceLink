/*
 * BruceLink shared core — COBS framing for byte-stream transports.
 *
 * Consistent Overhead Byte Stuffing (Cheshire & Baker). Each encoded
 * frame contains no 0x00 byte; a 0x00 delimiter is appended so a reader
 * can resynchronise on a serial line after corruption. Used by the
 * serial transport; BLE does not need it (GATT writes are framed).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Worst-case COBS overhead is 1 byte per 254, plus the leading code
 * byte and the trailing delimiter. */
#define BL_COBS_MAX_ENCODED(n) ((n) + (n) / 254u + 2u)

/*
 * COBS-encode `in` (in_len bytes) to `out`, appending a 0x00 delimiter.
 * Returns encoded length (including delimiter) or negative bl_result_t.
 */
int bl_cobs_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);

/*
 * COBS-decode one frame from `in` (must NOT include the delimiter).
 * Returns decoded length or negative bl_result_t (BL_E_FORMAT on a
 * malformed code sequence).
 */
int bl_cobs_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap);

/* Incremental stream decoder: feed bytes, get frames at each 0x00. */
typedef struct {
    uint8_t buf[BL_COBS_MAX_ENCODED(BL_MAX_MSG)];
    size_t len;
    bool overflow; /* current frame exceeded buffer; drop to next delimiter */
} bl_cobs_stream_t;

void bl_cobs_stream_init(bl_cobs_stream_t* s);

/*
 * Push one received byte. When a full frame (delimiter reached) decodes
 * successfully, returns BL_OK and sets out_frame/out_len (pointing
 * into caller-provided scratch `out`, out_cap >= BL_MAX_MSG). Returns
 * BL_E_SHORT when more bytes are needed, or BL_E_FORMAT/BL_E_OVERFLOW
 * on a bad or oversized frame (stream resynchronises on the next
 * delimiter).
 */
int bl_cobs_stream_push(
    bl_cobs_stream_t* s,
    uint8_t byte,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);

#ifdef __cplusplus
}
#endif
