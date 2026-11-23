/*
 * BruceLink shared core — PCAP (libpcap) writer + CRC32 integrity.
 *
 * Standards-compliant classic pcap: 24-byte global header then
 * 16-byte per-record headers. Bruce writes LINKTYPE_IEEE802_11 (105)
 * with no radiotap, snaplen 2500 — BruceLink matches that so captured
 * files open in Wireshark unchanged. All fields are host-endian in the
 * libpcap "a1b2c3d4" magic convention (readers detect byte order from
 * the magic); we emit little-endian.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_PCAP_MAGIC 0xa1b2c3d4u
#define BL_PCAP_GLOBAL_LEN 24u
#define BL_PCAP_REC_LEN 16u
#define BL_PCAP_LINKTYPE_IEEE802_11 105u
#define BL_PCAP_DEFAULT_SNAPLEN 2500u

/* Write the 24-byte global header (LE) to out (cap >= 24). */
int bl_pcap_global_header(uint32_t snaplen, uint32_t linktype, uint8_t* out, size_t cap);

/* Write the 16-byte per-record header (LE). incl_len is the number of
 * captured bytes that follow; orig_len the original on-air length. */
int bl_pcap_record_header(
    uint32_t ts_sec,
    uint32_t ts_usec,
    uint32_t incl_len,
    uint32_t orig_len,
    uint8_t* out,
    size_t cap);

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) for file integrity.
 * Streamable: start with crc = bl_crc32_init(), fold with _update,
 * finish with _final. */
uint32_t bl_crc32_init(void);
uint32_t bl_crc32_update(uint32_t crc, const void* data, size_t len);
uint32_t bl_crc32_final(uint32_t crc);
uint32_t bl_crc32(const void* data, size_t len); /* one-shot */

#ifdef __cplusplus
}
#endif
