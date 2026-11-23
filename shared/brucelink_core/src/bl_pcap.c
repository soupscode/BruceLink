/*
 * BruceLink shared core — PCAP writer + CRC32.
 * SPDX-License-Identifier: MIT
 */
#include "bl_pcap.h"

static void put_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

int bl_pcap_global_header(uint32_t snaplen, uint32_t linktype, uint8_t* out, size_t cap) {
    if(out == NULL) return BL_E_ARG;
    if(cap < BL_PCAP_GLOBAL_LEN) return BL_E_OVERFLOW;
    put_u32le(out + 0, BL_PCAP_MAGIC);
    put_u16le(out + 4, 2); /* version major */
    put_u16le(out + 6, 4); /* version minor */
    put_u32le(out + 8, 0); /* thiszone */
    put_u32le(out + 12, 0); /* sigfigs */
    put_u32le(out + 16, snaplen);
    put_u32le(out + 20, linktype);
    return (int)BL_PCAP_GLOBAL_LEN;
}

int bl_pcap_record_header(
    uint32_t ts_sec,
    uint32_t ts_usec,
    uint32_t incl_len,
    uint32_t orig_len,
    uint8_t* out,
    size_t cap) {
    if(out == NULL) return BL_E_ARG;
    if(cap < BL_PCAP_REC_LEN) return BL_E_OVERFLOW;
    put_u32le(out + 0, ts_sec);
    put_u32le(out + 4, ts_usec);
    put_u32le(out + 8, incl_len);
    put_u32le(out + 12, orig_len);
    return (int)BL_PCAP_REC_LEN;
}

/* --- CRC32 (reflected, poly 0xEDB88320), table-free Sarwate variant --- */

uint32_t bl_crc32_init(void) {
    return 0xFFFFFFFFu;
}

uint32_t bl_crc32_update(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for(size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for(int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

uint32_t bl_crc32_final(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

uint32_t bl_crc32(const void* data, size_t len) {
    return bl_crc32_final(bl_crc32_update(bl_crc32_init(), data, len));
}
