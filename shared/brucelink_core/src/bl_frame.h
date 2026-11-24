/*
 * BruceLink shared core — frame codec (header + payload + MAC).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_MAC_TAG_LEN 16u /* truncated HMAC-SHA256 */

typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t flags;
    uint32_t seq;
    uint16_t payload_len;
    const uint8_t* payload; /* points into the source buffer on decode */
    /* Request tag (header byte 5, covered by the MAC). A request carries a
     * non-zero tag; its response echoes it, so the requester can drop a
     * late response to an earlier request. 0 elsewhere. Last member, so
     * positional initialisers of the older fields keep working. */
    uint8_t tag;
} bl_frame_t;

/*
 * Encode a frame into out. If mac_key is non-NULL, appends a
 * MAC_TAG_LEN authentication tag computed over header||payload;
 * otherwise appends MAC_TAG_LEN zero bytes (handshake frames).
 * Returns the total encoded length, or a negative bl_result_t.
 */
int bl_frame_encode(
    const bl_frame_t* f,
    const uint8_t* mac_key,
    size_t mac_key_len,
    uint8_t* out,
    size_t out_cap);

/*
 * Decode and validate a frame from buf. Validates magic, version,
 * declared length and total size. If mac_key is non-NULL, verifies the
 * MAC in constant time and returns BL_E_MAC on mismatch. On success
 * fills *f (payload points into buf) and returns BL_OK.
 */
int bl_frame_decode(
    const uint8_t* buf,
    size_t len,
    const uint8_t* mac_key,
    size_t mac_key_len,
    bl_frame_t* f);

/* Total wire size of a frame with the given payload length. */
static inline size_t bl_frame_size(uint16_t payload_len) {
    return BL_HDR_LEN + (size_t)payload_len + BL_MAC_TAG_LEN;
}

#ifdef __cplusplus
}
#endif
