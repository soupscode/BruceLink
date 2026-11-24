/*
 * BruceLink shared core — capture files (see bl_files.h).
 * SPDX-License-Identifier: MIT
 */
#include "bl_files.h"

#include "bl_pcap.h" /* bl_crc32_* */
#include "bl_proto.h"

#include <string.h>

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}
static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static const char kExt[] = ".pcap";

bool bl_file_name_ok(const char* name, size_t len) {
    const size_t ext = sizeof(kExt) - 1;
    if(name == NULL || len <= ext || len > BL_FILE_NAME_MAX) return false;
    if(name[0] == '.') return false;
    for(size_t i = 0; i < len; i++) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-';
        if(!ok) return false;
    }
    return memcmp(name + len - ext, kExt, ext) == 0;
}

/* ---- names ---- */

int bl_files_encode_name(const char* name, size_t name_len, uint8_t* out, size_t cap) {
    if(out == NULL || !bl_file_name_ok(name, name_len)) return BL_E_ARG;
    if(cap < 1 + name_len) return BL_E_OVERFLOW;
    out[0] = (uint8_t)name_len;
    memcpy(out + 1, name, name_len);
    return (int)(1 + name_len);
}

int bl_files_parse_name(const uint8_t* p, size_t n, const char** name, size_t* name_len) {
    if(p == NULL || name == NULL || name_len == NULL) return BL_E_ARG;
    if(n < 1) return BL_E_SHORT;
    const size_t l = p[0];
    if(n != 1 + l) return BL_E_FORMAT;
    if(!bl_file_name_ok((const char*)(p + 1), l)) return BL_E_FORMAT;
    *name = (const char*)(p + 1);
    *name_len = l;
    return BL_OK;
}

/* ---- CAP_LIST ---- */

int bl_files_encode_list_req(uint8_t start, uint8_t* out, size_t cap) {
    if(out == NULL) return BL_E_ARG;
    if(cap < 1) return BL_E_OVERFLOW;
    out[0] = start;
    return 1;
}

int bl_files_parse_list_req(const uint8_t* p, size_t n, uint8_t* start) {
    if(p == NULL || start == NULL) return BL_E_ARG;
    if(n != 1) return BL_E_FORMAT;
    *start = p[0];
    return BL_OK;
}

int bl_files_list_begin(uint8_t total, uint8_t start, uint8_t* out, size_t cap, size_t* len) {
    if(out == NULL || len == NULL) return BL_E_ARG;
    if(cap < 3) return BL_E_OVERFLOW;
    out[0] = total;
    out[1] = start;
    out[2] = 0;
    *len = 3;
    return BL_OK;
}

int bl_files_list_add(const char* name, size_t name_len, uint32_t size, uint8_t* out, size_t cap, size_t* len) {
    if(out == NULL || len == NULL || *len < 3) return BL_E_ARG;
    if(!bl_file_name_ok(name, name_len)) return BL_E_ARG;
    const size_t need = 4 + 1 + name_len;
    if(out[2] >= BL_FILE_LIST_PAGE || *len + need > cap || *len + need > BL_MAX_PAYLOAD) return BL_E_OVERFLOW;
    uint8_t* e = out + *len;
    put_le32(e, size);
    e[4] = (uint8_t)name_len;
    memcpy(e + 5, name, name_len);
    *len += need;
    out[2]++;
    return BL_OK;
}

int bl_files_parse_list(
    const uint8_t* p,
    size_t n,
    uint8_t* total,
    uint8_t* start,
    bl_file_entry_t* entries,
    size_t max_entries,
    size_t* count) {
    if(p == NULL || total == NULL || start == NULL || count == NULL || (max_entries > 0 && entries == NULL))
        return BL_E_ARG;
    if(n < 3) return BL_E_SHORT;
    const uint8_t c = p[2];
    if(c > BL_FILE_LIST_PAGE) return BL_E_FORMAT;
    /* page past the total; an empty page past it is fine (the list shrank
     * between two pages) */
    if(c > 0 && (size_t)p[1] + c > p[0]) return BL_E_FORMAT;
    size_t off = 3;
    size_t got = 0;
    for(uint8_t i = 0; i < c; i++) {
        if(n - off < 5) return BL_E_SHORT;
        const uint32_t size = get_le32(p + off);
        const size_t l = p[off + 4];
        off += 5;
        if(n - off < l) return BL_E_SHORT;
        if(!bl_file_name_ok((const char*)(p + off), l)) return BL_E_FORMAT;
        if(got < max_entries) {
            memcpy(entries[got].name, p + off, l);
            entries[got].name[l] = '\0';
            entries[got].name_len = (uint8_t)l;
            entries[got].size = size;
            got++;
        }
        off += l;
    }
    if(off != n) return BL_E_FORMAT;
    *total = p[0];
    *start = p[1];
    *count = got;
    return BL_OK;
}

/* ---- CAP_FILE_INFO ---- */

int bl_files_encode_info(
    uint32_t size,
    uint32_t crc,
    const char* name,
    size_t name_len,
    uint8_t* out,
    size_t cap) {
    if(out == NULL || !bl_file_name_ok(name, name_len)) return BL_E_ARG;
    if(cap < 9 + name_len) return BL_E_OVERFLOW;
    put_le32(out, size);
    put_le32(out + 4, crc);
    out[8] = (uint8_t)name_len;
    memcpy(out + 9, name, name_len);
    return (int)(9 + name_len);
}

int bl_files_parse_info(
    const uint8_t* p,
    size_t n,
    uint32_t* size,
    uint32_t* crc,
    const char** name,
    size_t* name_len) {
    if(p == NULL || size == NULL || crc == NULL || name == NULL || name_len == NULL) return BL_E_ARG;
    if(n < 9) return BL_E_SHORT;
    int rc = bl_files_parse_name(p + 8, n - 8, name, name_len);
    if(rc != BL_OK) return rc;
    *size = get_le32(p);
    *crc = get_le32(p + 4);
    return BL_OK;
}

/* ---- CAP_XFER_DATA ---- */

int bl_files_encode_read_req(
    uint32_t offset,
    uint16_t max_len,
    const char* name,
    size_t name_len,
    uint8_t* out,
    size_t cap) {
    if(out == NULL || !bl_file_name_ok(name, name_len)) return BL_E_ARG;
    if(max_len == 0 || max_len > BL_FILE_CHUNK_MAX) return BL_E_ARG;
    if(cap < 7 + name_len) return BL_E_OVERFLOW;
    put_le32(out, offset);
    put_le16(out + 4, max_len);
    out[6] = (uint8_t)name_len;
    memcpy(out + 7, name, name_len);
    return (int)(7 + name_len);
}

int bl_files_parse_read_req(
    const uint8_t* p,
    size_t n,
    uint32_t* offset,
    uint16_t* max_len,
    const char** name,
    size_t* name_len) {
    if(p == NULL || offset == NULL || max_len == NULL || name == NULL || name_len == NULL) return BL_E_ARG;
    if(n < 7) return BL_E_SHORT;
    const uint16_t m = get_le16(p + 4);
    if(m == 0 || m > BL_FILE_CHUNK_MAX) return BL_E_FORMAT;
    int rc = bl_files_parse_name(p + 6, n - 6, name, name_len);
    if(rc != BL_OK) return rc;
    *offset = get_le32(p);
    *max_len = m;
    return BL_OK;
}

int bl_files_encode_data_head(uint32_t offset, size_t data_len, uint8_t* out, size_t cap) {
    if(out == NULL) return BL_E_ARG;
    if(data_len > BL_FILE_CHUNK_MAX) return BL_E_TOO_LARGE;
    if(cap < 4 + data_len) return BL_E_OVERFLOW;
    put_le32(out, offset);
    return (int)(4 + data_len);
}

int bl_files_parse_data(
    const uint8_t* p,
    size_t n,
    uint32_t* offset,
    const uint8_t** data,
    size_t* data_len) {
    if(p == NULL || offset == NULL || data == NULL || data_len == NULL) return BL_E_ARG;
    if(n < 4) return BL_E_SHORT;
    if(n - 4 > BL_FILE_CHUNK_MAX) return BL_E_TOO_LARGE;
    *offset = get_le32(p);
    *data = p + 4;
    *data_len = n - 4;
    return BL_OK;
}

/* ---- Flipper-side download ---- */

static void xfer_fail(bl_xferc_t* x, bl_xfer_fail_t why, uint16_t err) {
    x->state = BL_XFER_FAILED;
    x->fail = why;
    x->err = err;
    x->waiting = false;
}

void bl_xferc_init(bl_xferc_t* x) {
    if(x == NULL) return;
    memset(x, 0, sizeof(*x));
}

int bl_xferc_start(bl_xferc_t* x, const char* name, size_t name_len) {
    if(x == NULL || !bl_file_name_ok(name, name_len)) return BL_E_ARG;
    bl_xferc_init(x);
    memcpy(x->name, name, name_len);
    x->name[name_len] = '\0';
    x->name_len = (uint8_t)name_len;
    x->crc = bl_crc32_init();
    x->state = BL_XFER_INFO;
    return BL_OK;
}

bool bl_xferc_active(const bl_xferc_t* x) {
    return x != NULL && (x->state == BL_XFER_INFO || x->state == BL_XFER_DATA);
}

int bl_xferc_next(bl_xferc_t* x, uint8_t* type, uint8_t* out, size_t cap) {
    if(x == NULL || type == NULL || out == NULL) return BL_E_ARG;
    if(!bl_xferc_active(x) || x->waiting) return 0;
    int n;
    if(x->state == BL_XFER_INFO) {
        n = bl_files_encode_name(x->name, x->name_len, out, cap);
        *type = BL_MSG_CAP_FILE_INFO;
    } else {
        const uint32_t left = x->size - x->offset;
        const uint16_t want = (uint16_t)(left < BL_FILE_CHUNK_MAX ? left : BL_FILE_CHUNK_MAX);
        n = bl_files_encode_read_req(x->offset, want, x->name, x->name_len, out, cap);
        *type = BL_MSG_CAP_XFER_DATA;
        x->want_offset = x->offset;
        x->want_len = want;
    }
    if(n < 0) return n;
    x->waiting = true;
    x->waiting_type = *type;
    return n;
}

void bl_xferc_unsend(bl_xferc_t* x) {
    if(x != NULL) x->waiting = false;
}

void bl_xferc_retry(bl_xferc_t* x) {
    if(x == NULL || !bl_xferc_active(x)) return;
    x->waiting = false;
    if(++x->retries >= BL_XFER_RETRY_MAX) {
        xfer_fail(x, BL_XFER_F_GAVE_UP, 0);
        return;
    }
    /* The file may have changed while the link was away: check it again
     * before reading on (bl_xferc_on_response compares size and CRC). */
    x->state = BL_XFER_INFO;
}

/* All bytes in: the CRC decides. */
static void xfer_check_end(bl_xferc_t* x) {
    if(x->offset < x->size) return;
    if(bl_crc32_final(x->crc) == x->crc_expect) {
        x->state = BL_XFER_DONE;
    } else {
        xfer_fail(x, BL_XFER_F_CRC, 0);
    }
}

bool bl_xferc_on_response(
    bl_xferc_t* x,
    uint8_t type,
    const uint8_t* payload,
    size_t len,
    const uint8_t** data,
    size_t* data_len,
    uint32_t* at) {
    if(x == NULL || data == NULL || data_len == NULL || at == NULL) return false;
    *data = NULL;
    *data_len = 0;
    *at = 0;
    if(!bl_xferc_active(x) || !x->waiting) return false;
    if(payload == NULL && len > 0) return false;
    x->waiting = false;
    if(type == BL_MSG_ERROR) {
        const uint16_t e = len >= 2 ? get_le16(payload) : (uint16_t)BL_ERR_INTERNAL;
        xfer_fail(x, BL_XFER_F_REFUSED, e);
        return true;
    }
    if(type != x->waiting_type) {
        xfer_fail(x, BL_XFER_F_PROTOCOL, 0);
        return true;
    }
    if(type == BL_MSG_CAP_FILE_INFO) {
        uint32_t size = 0, crc = 0;
        const char* name = NULL;
        size_t nl = 0;
        if(bl_files_parse_info(payload, len, &size, &crc, &name, &nl) != BL_OK || nl != x->name_len ||
           memcmp(name, x->name, nl) != 0) {
            xfer_fail(x, BL_XFER_F_PROTOCOL, 0);
            return true;
        }
        if(size > BL_FILE_SIZE_MAX) {
            xfer_fail(x, BL_XFER_F_PROTOCOL, 0);
            return true;
        }
        if(x->have_info && (size != x->size || crc != x->crc_expect)) {
            xfer_fail(x, BL_XFER_F_CHANGED, 0);
            return true;
        }
        x->have_info = true;
        x->size = size;
        x->crc_expect = crc;
        x->state = BL_XFER_DATA; /* not progress: only bytes reset the retry count */
        xfer_check_end(x); /* an empty file is complete at once */
        return true;
    }
    /* CAP_XFER_DATA: exactly the bytes asked for, at the offset asked */
    uint32_t off = 0;
    const uint8_t* d = NULL;
    size_t dl = 0;
    if(bl_files_parse_data(payload, len, &off, &d, &dl) != BL_OK || off != x->want_offset ||
       dl != x->want_len) {
        xfer_fail(x, BL_XFER_F_PROTOCOL, 0);
        return true;
    }
    x->crc = bl_crc32_update(x->crc, d, dl);
    *data = d;
    *data_len = dl;
    *at = off;
    x->offset += (uint32_t)dl;
    x->retries = 0;
    xfer_check_end(x);
    return true;
}
