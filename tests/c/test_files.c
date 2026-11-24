/* bl_files: capture file names, payloads and the Flipper-side download.
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_files.h"
#include "../../shared/brucelink_core/src/bl_pcap.h"
#include "../../shared/brucelink_core/src/bl_proto.h"
#include "bl_test.h"

#include <string.h>

static bool ok(const char* s) {
    return bl_file_name_ok(s, strlen(s));
}

static void t_names(void) {
    /* what Bruce's sniffer writes */
    CHECK(ok("HS_A1B2C3D4E5F6.pcap"));
    CHECK(ok("HS_A1B2C3D4E5F6_My-Home_Wifi.5G.pcap"));
    CHECK(ok("HS_A1B2C3D4E5F6_abcdefghijklmnopqrstuvwxyz012345.pcap")); /* 32-char SSID */
    /* never a path, never outside the directory */
    CHECK(!ok("../HS_A.pcap"));
    CHECK(!ok("..pcap"));
    CHECK(!ok("a..b.pcap"));
    CHECK(!ok("dir/HS_A.pcap"));
    CHECK(!ok("dir\\HS_A.pcap"));
    CHECK(!ok("/HS_A.pcap"));
    CHECK(!ok(".hidden.pcap"));
    CHECK(!ok(".pcap"));
    CHECK(!ok(""));
    CHECK(!ok("bruce.conf"));
    CHECK(!ok("HS_A.pcap.txt"));
    CHECK(!ok("HS_A.PCAP"));
    CHECK(!ok("HS A.pcap"));
    CHECK(!ok("HS_\x01.pcap"));
    CHECK(!ok("HS_\xc3\xa9.pcap"));
    CHECK(!bl_file_name_ok(NULL, 5));
    /* an embedded NUL is not a shorter name */
    CHECK(!bl_file_name_ok("HS_A\0.pcap", 10));
    char longest[BL_FILE_NAME_MAX + 2];
    memset(longest, 'a', sizeof(longest));
    memcpy(longest + BL_FILE_NAME_MAX - 5, ".pcap", 5);
    CHECK(bl_file_name_ok(longest, BL_FILE_NAME_MAX));
    memcpy(longest + BL_FILE_NAME_MAX - 4, ".pcap", 5);
    CHECK(!bl_file_name_ok(longest, BL_FILE_NAME_MAX + 1));
}

static void t_list(void) {
    uint8_t out[BL_MAX_PAYLOAD];
    size_t len = 0;
    CHECK_EQ_INT(bl_files_list_begin(9, 0, out, sizeof(out), &len), BL_OK);
    char name[BL_FILE_NAME_MAX + 1];
    for(int i = 0; i < (int)BL_FILE_LIST_PAGE; i++) {
        /* the longest names: a full page still fits one payload */
        memset(name, 'A' + i, BL_FILE_NAME_MAX);
        memcpy(name + BL_FILE_NAME_MAX - 5, ".pcap", 5);
        CHECK_EQ_INT(bl_files_list_add(name, BL_FILE_NAME_MAX, 1000u + (uint32_t)i, out, sizeof(out), &len), BL_OK);
    }
    CHECK(len <= BL_MAX_PAYLOAD);
    CHECK_EQ_INT(bl_files_list_add("HS_X.pcap", 9, 1, out, sizeof(out), &len), BL_E_OVERFLOW);
    bl_file_entry_t e[BL_FILE_LIST_PAGE];
    uint8_t total = 0, start = 0;
    size_t count = 0;
    CHECK_EQ_INT(bl_files_parse_list(out, len, &total, &start, e, BL_FILE_LIST_PAGE, &count), BL_OK);
    CHECK_EQ_INT(total, 9);
    CHECK_EQ_INT(start, 0);
    CHECK_EQ_INT((long)count, (long)BL_FILE_LIST_PAGE);
    CHECK_EQ_INT((long)e[6].size, 1006);
    CHECK_EQ_INT(e[6].name_len, BL_FILE_NAME_MAX);
    CHECK_EQ_INT((long)strlen(e[6].name), (long)BL_FILE_NAME_MAX);
    /* a name the StickS3 must never serve is not even listed */
    CHECK_EQ_INT(bl_files_list_begin(1, 0, out, sizeof(out), &len), BL_OK);
    CHECK_EQ_INT(bl_files_list_add("../bruce.conf", 13, 5, out, sizeof(out), &len), BL_E_ARG);
    CHECK_EQ_INT(out[2], 0);
    /* a Flipper never accepts one either, nor a page past the total */
    CHECK_EQ_INT(bl_files_list_begin(1, 0, out, sizeof(out), &len), BL_OK);
    CHECK_EQ_INT(bl_files_list_add("HS_A.pcap", 9, 5, out, sizeof(out), &len), BL_OK);
    out[3 + 5 + 3] = '/'; /* HS_/.pcap */
    CHECK_EQ_INT(bl_files_parse_list(out, len, &total, &start, e, 7, &count), BL_E_FORMAT);
    out[3 + 5 + 3] = 'A';
    out[1] = 1; /* start 1 + 1 entry > total 1 */
    CHECK_EQ_INT(bl_files_parse_list(out, len, &total, &start, e, 7, &count), BL_E_FORMAT);
    out[1] = 0;
    CHECK_EQ_INT(bl_files_parse_list(out, len - 1, &total, &start, e, 7, &count), BL_E_SHORT);
    uint8_t extra[BL_MAX_PAYLOAD];
    memcpy(extra, out, len);
    extra[len] = 0;
    CHECK_EQ_INT(bl_files_parse_list(extra, len + 1, &total, &start, e, 7, &count), BL_E_FORMAT);
    /* requests */
    uint8_t r[2];
    uint8_t st = 0;
    CHECK_EQ_INT(bl_files_encode_list_req(7, r, sizeof(r)), 1);
    CHECK_EQ_INT(bl_files_parse_list_req(r, 1, &st), BL_OK);
    CHECK_EQ_INT(st, 7);
    CHECK_EQ_INT(bl_files_parse_list_req(r, 2, &st), BL_E_FORMAT);
}

static void t_info_read_data(void) {
    uint8_t out[BL_MAX_PAYLOAD];
    const char* nm = NULL;
    size_t nl = 0;
    uint32_t size = 0, crc = 0, off = 0;
    uint16_t max = 0;
    int n = bl_files_encode_info(1234, 0xDEADBEEFu, "HS_A.pcap", 9, out, sizeof(out));
    CHECK_EQ_INT(n, 18);
    CHECK_EQ_INT(bl_files_parse_info(out, (size_t)n, &size, &crc, &nm, &nl), BL_OK);
    CHECK_EQ_INT((long)size, 1234);
    CHECK(crc == 0xDEADBEEFu);
    CHECK(nl == 9 && memcmp(nm, "HS_A.pcap", 9) == 0);
    CHECK_EQ_INT(bl_files_encode_info(1, 1, "a/b.pcap", 8, out, sizeof(out)), BL_E_ARG);

    n = bl_files_encode_read_req(448, 448, "HS_A.pcap", 9, out, sizeof(out));
    CHECK_EQ_INT(n, 16);
    CHECK_EQ_INT(bl_files_parse_read_req(out, (size_t)n, &off, &max, &nm, &nl), BL_OK);
    CHECK_EQ_INT((long)off, 448);
    CHECK_EQ_INT(max, 448);
    CHECK_EQ_INT(bl_files_encode_read_req(0, 0, "HS_A.pcap", 9, out, sizeof(out)), BL_E_ARG);
    CHECK_EQ_INT(bl_files_encode_read_req(0, BL_FILE_CHUNK_MAX + 1, "HS_A.pcap", 9, out, sizeof(out)), BL_E_ARG);
    out[4] = 0xFF; /* max_len 0x01FF > chunk max */
    CHECK_EQ_INT(bl_files_parse_read_req(out, (size_t)n, &off, &max, &nm, &nl), BL_E_FORMAT);
    /* a name length that disagrees with the payload */
    n = bl_files_encode_name("HS_A.pcap", 9, out, sizeof(out));
    CHECK_EQ_INT(n, 10);
    out[0] = 8;
    CHECK_EQ_INT(bl_files_parse_name(out, (size_t)n, &nm, &nl), BL_E_FORMAT);
    out[0] = 200;
    CHECK_EQ_INT(bl_files_parse_name(out, (size_t)n, &nm, &nl), BL_E_FORMAT);

    const uint8_t* d = NULL;
    size_t dl = 0;
    n = bl_files_encode_data_head(896, 3, out, sizeof(out));
    CHECK_EQ_INT(n, 7);
    memcpy(out + 4, "abc", 3);
    CHECK_EQ_INT(bl_files_parse_data(out, 7, &off, &d, &dl), BL_OK);
    CHECK(off == 896 && dl == 3 && memcmp(d, "abc", 3) == 0);
    CHECK_EQ_INT(bl_files_encode_data_head(0, BL_FILE_CHUNK_MAX + 1, out, sizeof(out)), BL_E_TOO_LARGE);
    CHECK_EQ_INT(bl_files_parse_data(out, 4 + BL_FILE_CHUNK_MAX + 1, &off, &d, &dl), BL_E_TOO_LARGE);
    CHECK_EQ_INT(bl_files_parse_data(out, 3, &off, &d, &dl), BL_E_SHORT);
}

/* ---- the download, against a file served from memory ---- */

typedef struct {
    uint8_t file[2000];
    uint32_t size;
    char got[2000];
    uint32_t got_len;
} srv_t;

/* Answer one request the way the StickS3 does. */
static size_t serve(const srv_t* s, uint8_t type, const uint8_t* req, size_t rl, uint8_t* resp) {
    const char* nm = NULL;
    size_t nl = 0;
    if(type == BL_MSG_CAP_FILE_INFO) {
        if(bl_files_parse_name(req, rl, &nm, &nl) != BL_OK) return 0;
        return (size_t)bl_files_encode_info(s->size, bl_crc32(s->file, s->size), nm, nl, resp, BL_MAX_PAYLOAD);
    }
    uint32_t off = 0;
    uint16_t max = 0;
    if(bl_files_parse_read_req(req, rl, &off, &max, &nm, &nl) != BL_OK || off > s->size) return 0;
    size_t n = s->size - off < max ? s->size - off : max;
    int h = bl_files_encode_data_head(off, n, resp, BL_MAX_PAYLOAD);
    memcpy(resp + 4, s->file + off, n);
    return (size_t)h;
}

/* One request/response round; false when nothing was due. */
static bool round_trip(bl_xferc_t* x, srv_t* s) {
    uint8_t type = 0, req[BL_MAX_PAYLOAD], resp[BL_MAX_PAYLOAD];
    int n = bl_xferc_next(x, &type, req, sizeof(req));
    if(n <= 0) return false;
    size_t rl = serve(s, type, req, (size_t)n, resp);
    const uint8_t* d = NULL;
    size_t dl = 0;
    uint32_t at = 0;
    CHECK(bl_xferc_on_response(x, type, resp, rl, &d, &dl, &at));
    if(dl) {
        CHECK_EQ_INT((long)at, (long)s->got_len); /* always appended in order */
        memcpy(s->got + s->got_len, d, dl);
        s->got_len += (uint32_t)dl;
    }
    return true;
}

static void t_download(void) {
    static srv_t s;
    memset(&s, 0, sizeof(s));
    for(int i = 0; i < 1500; i++) s.file[i] = (uint8_t)(i * 7);
    s.size = 1500; /* 4 chunks: 448 448 448 156 */
    bl_xferc_t x;
    CHECK_EQ_INT(bl_xferc_start(&x, "HS_A1B2C3D4E5F6_Home.pcap", 25), BL_OK);
    CHECK_EQ_INT(bl_xferc_start(&x, "../x.pcap", 9), BL_E_ARG);
    CHECK_EQ_INT(bl_xferc_start(&x, "HS_A1B2C3D4E5F6_Home.pcap", 25), BL_OK);
    int rounds = 0;
    while(round_trip(&x, &s)) rounds++;
    CHECK_EQ_INT(rounds, 5); /* info + 4 chunks */
    CHECK_EQ_INT(x.state, BL_XFER_DONE);
    CHECK_EQ_INT((long)s.got_len, 1500);
    CHECK(memcmp(s.got, s.file, 1500) == 0);
    /* one request at a time */
    uint8_t type = 0, buf[BL_MAX_PAYLOAD];
    CHECK_EQ_INT(bl_xferc_next(&x, &type, buf, sizeof(buf)), 0);

    /* an empty file is done after the info */
    srv_t e;
    memset(&e, 0, sizeof(e));
    CHECK_EQ_INT(bl_xferc_start(&x, "HS_E.pcap", 9), BL_OK);
    CHECK(round_trip(&x, &e));
    CHECK_EQ_INT(x.state, BL_XFER_DONE);
}

static void t_download_resume_and_failures(void) {
    static srv_t s;
    memset(&s, 0, sizeof(s));
    for(int i = 0; i < 1000; i++) s.file[i] = (uint8_t)(255 - i);
    s.size = 1000;
    bl_xferc_t x;
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(round_trip(&x, &s)); /* info */
    CHECK(round_trip(&x, &s)); /* 448 */
    /* the next request goes out, then the link drops */
    uint8_t type = 0, req[BL_MAX_PAYLOAD];
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    bl_xferc_retry(&x);
    CHECK_EQ_INT(x.state, BL_XFER_INFO); /* asks again: is it the same file? */
    CHECK_EQ_INT((long)x.offset, 448);
    while(round_trip(&x, &s)) {
    }
    CHECK_EQ_INT(x.state, BL_XFER_DONE);
    CHECK(s.got_len == 1000 && memcmp(s.got, s.file, 1000) == 0);

    /* the file changed while the link was away */
    memset(s.got, 0, sizeof(s.got));
    s.got_len = 0;
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(round_trip(&x, &s));
    CHECK(round_trip(&x, &s));
    bl_xferc_retry(&x);
    s.size = 1001;
    CHECK(round_trip(&x, &s));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_CHANGED);

    /* a reply for the wrong offset, or a different length */
    s.size = 1000;
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(round_trip(&x, &s));
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    uint8_t resp[BL_MAX_PAYLOAD];
    int h = bl_files_encode_data_head(4, 448, resp, sizeof(resp));
    const uint8_t* d = NULL;
    size_t dl = 0;
    uint32_t at = 0;
    CHECK(bl_xferc_on_response(&x, BL_MSG_CAP_XFER_DATA, resp, (size_t)h, &d, &dl, &at));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_PROTOCOL);
    CHECK_EQ_INT((long)dl, 0);
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(round_trip(&x, &s));
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    h = bl_files_encode_data_head(0, 100, resp, sizeof(resp));
    CHECK(bl_xferc_on_response(&x, BL_MSG_CAP_XFER_DATA, resp, (size_t)h, &d, &dl, &at));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_PROTOCOL);

    /* an info reply for another file */
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    h = bl_files_encode_info(10, 0, "HS_Q.pcap", 9, resp, sizeof(resp));
    CHECK(bl_xferc_on_response(&x, BL_MSG_CAP_FILE_INFO, resp, (size_t)h, &d, &dl, &at));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_PROTOCOL);

    /* a file larger than any StickS3 could hold */
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    h = bl_files_encode_info(BL_FILE_SIZE_MAX + 1, 0, "HS_R.pcap", 9, resp, sizeof(resp));
    CHECK(bl_xferc_on_response(&x, BL_MSG_CAP_FILE_INFO, resp, (size_t)h, &d, &dl, &at));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_PROTOCOL);

    /* an error reply */
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    const uint8_t err[2] = {BL_ERR_NOT_FOUND, 0};
    CHECK(bl_xferc_on_response(&x, BL_MSG_ERROR, err, 2, &d, &dl, &at));
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_REFUSED && x.err == BL_ERR_NOT_FOUND);

    /* a reply with nothing outstanding is not the download's */
    bl_xferc_start(&x, "HS_R.pcap", 9);
    h = bl_files_encode_info(10, 0, "HS_R.pcap", 9, resp, sizeof(resp));
    CHECK(!bl_xferc_on_response(&x, BL_MSG_CAP_FILE_INFO, resp, (size_t)h, &d, &dl, &at));
    CHECK_EQ_INT(x.state, BL_XFER_INFO);
    /* a slot that was busy: the request is asked for again */
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);
    bl_xferc_unsend(&x);
    CHECK(bl_xferc_next(&x, &type, req, sizeof(req)) > 0);

    /* bytes that do not match the CRC */
    memset(s.got, 0, sizeof(s.got));
    s.got_len = 0;
    bl_xferc_start(&x, "HS_R.pcap", 9);
    CHECK(round_trip(&x, &s)); /* info with the real CRC */
    s.file[10] ^= 1; /* the data then differs */
    while(round_trip(&x, &s)) {
    }
    CHECK(x.state == BL_XFER_FAILED && x.fail == BL_XFER_F_CRC);
}

void run_files_tests(void) {
    RUN(t_names);
    RUN(t_list);
    RUN(t_info_read_data);
    RUN(t_download);
    RUN(t_download_resume_and_failures);
}
