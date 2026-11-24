/*
 * BruceLink shared core — capture files on the StickS3 (task P2.7).
 *
 * The StickS3 keeps captured handshakes as pcap files on its LittleFS
 * (Bruce's sniffer writes /BrucePCAP/handshakes/HS_<MAC>[_<SSID>].pcap). The
 * Flipper lists them, downloads them in chunks and can delete them, over the
 * authenticated session. Reading a file needs no radio, so these run on the
 * live link (ADR 0006 only moves radio work into jobs).
 *
 * Requests (C->P) and their responses (P->C, RESPONSE flag) use the same
 * message type:
 *   CAP_LIST       req  [u8 start]
 *                  resp [u8 total][u8 start][u8 count]
 *                       count x ([u32 size][u8 name_len][name])
 *   CAP_FILE_INFO  req  [u8 name_len][name]
 *                  resp [u32 size][u32 crc32][u8 name_len][name]
 *   CAP_XFER_DATA  req  [u32 offset][u16 max_len][u8 name_len][name]
 *                  resp [u32 offset][data...]        (data <= max_len)
 *   CAP_DELETE     req  [u8 name_len][name]
 *                  resp (empty)
 * All integers little-endian. Every read is stateless and names its offset,
 * so a download simply continues after a dropped link. The CRC-32 (the pcap
 * writer's, bl_pcap.h) of the whole file, sent with the file info, is
 * checked once the last byte is in.
 *
 * Names are the bare file name, never a path: bl_file_name_ok() is the only
 * gate, applied on both devices. It accepts what Bruce's sniffer produces
 * ([A-Za-z0-9._-], ending in ".pcap"; Bruce keeps dots in SSIDs, so "..",
 * as in "HS_<MAC>_Cafe..pcap", is allowed) and, with no separator possible
 * and no leading dot, can never name a file outside the capture directory.
 *
 * Not encrypted: until M4.6 the link authenticates frames but does not
 * encrypt them, so a capture file sent over it is readable by a nearby BLE
 * sniffer.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_FILE_NAME_MAX 63u /* "HS_" + 12 hex + "_" + 32 + ".pcap" = 53 */
#define BL_FILE_CHUNK_MAX 448u /* data bytes per CAP_XFER_DATA reply */
#define BL_FILE_LIST_PAGE 7u /* entries per CAP_LIST reply (fits 512 B) */
#define BL_FILE_LIST_MAX 255u
#define BL_FILE_SIZE_MAX (4u * 1024u * 1024u) /* larger than the StickS3's LittleFS */
/* The largest request payload (CAP_XFER_DATA with the longest name) and the
 * sealed frame it makes: what a sender's buffer must hold (review
 * 2026-11-23: the Flipper sealed into 44 bytes, so no request with a file
 * name could be sent). */
#define BL_FILES_REQ_PAYLOAD_MAX (7u + BL_FILE_NAME_MAX)
#define BL_FILES_REQ_FRAME_MAX (BL_HDR_LEN + BL_FILES_REQ_PAYLOAD_MAX + BL_MAC_LEN)
/* A download gives up after this many retries in a row with no progress. */
#define BL_XFER_RETRY_MAX 5u

/* A capture file name the StickS3 may serve: 1..BL_FILE_NAME_MAX bytes of
 * [A-Za-z0-9._-], not starting with '.', ending in ".pcap". */
bool bl_file_name_ok(const char* name, size_t len);

typedef struct {
    char name[BL_FILE_NAME_MAX + 1]; /* NUL-terminated */
    uint8_t name_len;
    uint32_t size;
} bl_file_entry_t;

/* ---- CAP_LIST ---- */
int bl_files_encode_list_req(uint8_t start, uint8_t* out, size_t cap);
int bl_files_parse_list_req(const uint8_t* p, size_t n, uint8_t* start);
/* Response, built in place: begin, then add up to BL_FILE_LIST_PAGE
 * entries. *len is the payload length so far. add returns BL_E_ARG for a
 * name bl_file_name_ok() refuses (the caller skips that file), BL_E_OVERFLOW
 * when the page is full. */
int bl_files_list_begin(uint8_t total, uint8_t start, uint8_t* out, size_t cap, size_t* len);
int bl_files_list_add(const char* name, size_t name_len, uint32_t size, uint8_t* out, size_t cap, size_t* len);
/* Parse a whole page into entries[0..*count). Every name is checked. */
int bl_files_parse_list(
    const uint8_t* p,
    size_t n,
    uint8_t* total,
    uint8_t* start,
    bl_file_entry_t* entries,
    size_t max_entries,
    size_t* count);

/* ---- CAP_FILE_INFO / CAP_DELETE request: [u8 name_len][name] ---- */
int bl_files_encode_name(const char* name, size_t name_len, uint8_t* out, size_t cap);
/* *name points into p (not NUL-terminated). */
int bl_files_parse_name(const uint8_t* p, size_t n, const char** name, size_t* name_len);

/* ---- CAP_FILE_INFO response ---- */
int bl_files_encode_info(
    uint32_t size,
    uint32_t crc,
    const char* name,
    size_t name_len,
    uint8_t* out,
    size_t cap);
int bl_files_parse_info(
    const uint8_t* p,
    size_t n,
    uint32_t* size,
    uint32_t* crc,
    const char** name,
    size_t* name_len);

/* ---- CAP_XFER_DATA ---- */
int bl_files_encode_read_req(
    uint32_t offset,
    uint16_t max_len,
    const char* name,
    size_t name_len,
    uint8_t* out,
    size_t cap);
int bl_files_parse_read_req(
    const uint8_t* p,
    size_t n,
    uint32_t* offset,
    uint16_t* max_len,
    const char** name,
    size_t* name_len);
/* Response: [u32 offset] then the data; the caller writes the data at
 * out + 4 and passes its length. */
int bl_files_encode_data_head(uint32_t offset, size_t data_len, uint8_t* out, size_t cap);
int bl_files_parse_data(
    const uint8_t* p,
    size_t n,
    uint32_t* offset,
    const uint8_t** data,
    size_t* data_len);

/* ---- Flipper side: one download ------------------------------------
 *
 *   start(name) -> next() gives CAP_FILE_INFO, then CAP_XFER_DATA requests,
 *   one at a time; on_response() hands each chunk to the caller to write at
 *   *at (always the current end: chunks arrive in order). Done when the
 *   last byte is in and the CRC matches.
 *
 * A dropped link or a timed-out request keeps the progress: the next
 * request after the link is back asks for the file info again and, if the
 * file is unchanged (same size and CRC), continues where it stopped; a
 * changed file fails the download (BL_XFER_F_CHANGED). */
typedef enum {
    BL_XFER_IDLE = 0,
    BL_XFER_INFO, /* asking for size + CRC */
    BL_XFER_DATA, /* reading chunks */
    BL_XFER_DONE,
    BL_XFER_FAILED,
} bl_xfer_state_t;

typedef enum {
    BL_XFER_F_NONE = 0,
    BL_XFER_F_REFUSED, /* an error reply; err holds the wire code */
    BL_XFER_F_PROTOCOL, /* a reply that does not fit the request */
    BL_XFER_F_CHANGED, /* the file changed while downloading */
    BL_XFER_F_CRC, /* all bytes in, CRC mismatch */
    BL_XFER_F_GAVE_UP, /* BL_XFER_RETRY_MAX retries in a row without progress */
} bl_xfer_fail_t;

typedef struct {
    bl_xfer_state_t state;
    bl_xfer_fail_t fail;
    uint16_t err;
    char name[BL_FILE_NAME_MAX + 1];
    uint8_t name_len;
    bool have_info; /* size / crc_expect are known */
    uint32_t size;
    uint32_t crc_expect;
    uint32_t offset; /* bytes received and handed out */
    uint32_t crc; /* running CRC over them (bl_crc32_update) */
    bool waiting; /* a request is out */
    uint8_t waiting_type;
    uint32_t want_offset; /* offset of the outstanding read */
    uint16_t want_len;
    uint8_t retries; /* in a row, since the last progress */
} bl_xferc_t;

void bl_xferc_init(bl_xferc_t* x);
int bl_xferc_start(bl_xferc_t* x, const char* name, size_t name_len);
/* The next request to send, if one is due: writes its type and payload and
 * returns the payload length; 0 when nothing is due; <0 on error. */
int bl_xferc_next(bl_xferc_t* x, uint8_t* type, uint8_t* out, size_t cap);
/* The request from bl_xferc_next() could not be sent (slot busy). */
void bl_xferc_unsend(bl_xferc_t* x);
/* A response arrived. Returns true if it answered this download's request.
 * For a chunk, *data / *data_len / *at say what to write (else *data_len
 * is 0). */
bool bl_xferc_on_response(
    bl_xferc_t* x,
    uint8_t type,
    const uint8_t* payload,
    size_t len,
    const uint8_t** data,
    size_t* data_len,
    uint32_t* at);
/* The request went unanswered (timeout) or the link dropped: ask again,
 * unless that has now happened BL_XFER_RETRY_MAX times in a row without a
 * byte of progress (then BL_XFER_F_GAVE_UP). Only for an active download. */
void bl_xferc_retry(bl_xferc_t* x);
bool bl_xferc_active(const bl_xferc_t* x);

#ifdef __cplusplus
}
#endif
