/*
 * BruceLink shared core — frame codec.
 * SPDX-License-Identifier: MIT
 */
#include "bl_frame.h"

#include "bl_crypto.h"

#include <string.h>

static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16le(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void frame_mac(
    const uint8_t* key,
    size_t key_len,
    const uint8_t* hdr_and_payload,
    size_t len,
    uint8_t tag[BL_MAC_TAG_LEN]) {
    uint8_t full[BL_SHA256_LEN];
    bl_hmac_sha256(key, key_len, hdr_and_payload, len, full);
    memcpy(tag, full, BL_MAC_TAG_LEN);
    bl_secure_zero(full, sizeof(full));
}

int bl_frame_encode(
    const bl_frame_t* f,
    const uint8_t* mac_key,
    size_t mac_key_len,
    uint8_t* out,
    size_t out_cap) {
    if(f == NULL || out == NULL) return BL_E_ARG;
    if(f->payload_len > BL_MAX_PAYLOAD) return BL_E_TOO_LARGE;
    if(f->payload_len > 0 && f->payload == NULL) return BL_E_ARG;
    size_t total = bl_frame_size(f->payload_len);
    if(out_cap < total) return BL_E_OVERFLOW;

    out[BL_OFF_MAGIC0] = BL_MAGIC0;
    out[BL_OFF_MAGIC1] = BL_MAGIC1;
    out[BL_OFF_VERSION] = f->version;
    out[BL_OFF_TYPE] = f->type;
    out[BL_OFF_FLAGS] = f->flags;
    out[BL_OFF_RSVD] = 0;
    put_u32le(out + BL_OFF_SEQ, f->seq);
    put_u16le(out + BL_OFF_LEN, f->payload_len);
    if(f->payload_len) memcpy(out + BL_HDR_LEN, f->payload, f->payload_len);

    uint8_t* tag = out + BL_HDR_LEN + f->payload_len;
    if(mac_key != NULL) {
        frame_mac(mac_key, mac_key_len, out, BL_HDR_LEN + f->payload_len, tag);
    } else {
        memset(tag, 0, BL_MAC_TAG_LEN);
    }
    return (int)total;
}

int bl_frame_decode(
    const uint8_t* buf,
    size_t len,
    const uint8_t* mac_key,
    size_t mac_key_len,
    bl_frame_t* f) {
    if(buf == NULL || f == NULL) return BL_E_ARG;
    if(len < BL_HDR_LEN + BL_MAC_TAG_LEN) return BL_E_SHORT;
    if(buf[BL_OFF_MAGIC0] != BL_MAGIC0 || buf[BL_OFF_MAGIC1] != BL_MAGIC1) return BL_E_FORMAT;

    uint16_t plen = get_u16le(buf + BL_OFF_LEN);
    if(plen > BL_MAX_PAYLOAD) return BL_E_TOO_LARGE;
    size_t total = bl_frame_size(plen);
    if(len != total) return BL_E_FORMAT;

    if(mac_key != NULL) {
        uint8_t tag[BL_MAC_TAG_LEN];
        frame_mac(mac_key, mac_key_len, buf, BL_HDR_LEN + plen, tag);
        const uint8_t* recv_tag = buf + BL_HDR_LEN + plen;
        if(!bl_ct_equal(tag, recv_tag, BL_MAC_TAG_LEN)) {
            bl_secure_zero(tag, sizeof(tag));
            return BL_E_MAC;
        }
        bl_secure_zero(tag, sizeof(tag));
    }

    f->version = buf[BL_OFF_VERSION];
    f->type = buf[BL_OFF_TYPE];
    f->flags = buf[BL_OFF_FLAGS];
    f->seq = get_u32le(buf + BL_OFF_SEQ);
    f->payload_len = plen;
    f->payload = (plen > 0) ? (buf + BL_HDR_LEN) : NULL;
    return BL_OK;
}
