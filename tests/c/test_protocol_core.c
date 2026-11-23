/* Frame, session, fragmentation and COBS behaviour tests.
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_cobs.h"
#include "../../shared/brucelink_core/src/bl_frame.h"
#include "../../shared/brucelink_core/src/bl_reasm.h"
#include "../../shared/brucelink_core/src/bl_session.h"
#include "bl_test.h"

/* --- deterministic RNG for reproducible handshakes --- */
typedef struct {
    uint32_t s;
} prng_t;
static void prng(void* ctx, uint8_t* buf, size_t len) {
    prng_t* p = (prng_t*)ctx;
    for(size_t i = 0; i < len; i++) {
        p->s = p->s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(p->s >> 24);
    }
}

/* ------------------------------------------------------------- frame */

static void t_frame_roundtrip_unauth(void) {
    uint8_t out[BL_MAX_MSG];
    uint8_t pl[5] = {1, 2, 3, 4, 5};
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_PING, 0, 42, 5, pl};
    int n = bl_frame_encode(&f, NULL, 0, out, sizeof(out));
    CHECK_EQ_INT(n, (int)bl_frame_size(5));
    bl_frame_t g;
    CHECK_EQ_INT(bl_frame_decode(out, n, NULL, 0, &g), BL_OK);
    CHECK_EQ_INT(g.type, BL_MSG_PING);
    CHECK_EQ_INT(g.seq, 42);
    CHECK_EQ_INT(g.payload_len, 5);
    CHECK(memcmp(g.payload, pl, 5) == 0);
}

static void t_frame_mac_roundtrip_and_tamper(void) {
    uint8_t key[BL_KEY_LEN];
    memset(key, 0x5a, sizeof(key));
    uint8_t out[BL_MAX_MSG];
    uint8_t pl[64];
    for(int i = 0; i < 64; i++) pl[i] = (uint8_t)i;
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_STATUS, BL_FLAG_RESPONSE, 7, 64, pl};
    int n = bl_frame_encode(&f, key, sizeof(key), out, sizeof(out));
    CHECK(n > 0);
    bl_frame_t g;
    CHECK_EQ_INT(bl_frame_decode(out, n, key, sizeof(key), &g), BL_OK);
    /* flip a payload bit -> MAC must fail */
    out[BL_HDR_LEN + 3] ^= 0x01;
    CHECK_EQ_INT(bl_frame_decode(out, n, key, sizeof(key), &g), BL_E_MAC);
    out[BL_HDR_LEN + 3] ^= 0x01;
    /* flip the tag -> fail */
    out[n - 1] ^= 0x80;
    CHECK_EQ_INT(bl_frame_decode(out, n, key, sizeof(key), &g), BL_E_MAC);
    /* wrong key -> fail */
    out[n - 1] ^= 0x80;
    uint8_t wrong[BL_KEY_LEN];
    memset(wrong, 0x5b, sizeof(wrong));
    CHECK_EQ_INT(bl_frame_decode(out, n, wrong, sizeof(wrong), &g), BL_E_MAC);
}

static void t_frame_malformed(void) {
    uint8_t out[BL_MAX_MSG];
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_PING, 0, 1, 0, NULL};
    int n = bl_frame_encode(&f, NULL, 0, out, sizeof(out));
    bl_frame_t g;
    /* too short */
    CHECK_EQ_INT(bl_frame_decode(out, 4, NULL, 0, &g), BL_E_SHORT);
    /* bad magic */
    out[0] = 'X';
    CHECK_EQ_INT(bl_frame_decode(out, n, NULL, 0, &g), BL_E_FORMAT);
    out[0] = BL_MAGIC0;
    /* declared length longer than buffer */
    out[BL_OFF_LEN] = 0xFF;
    CHECK_EQ_INT(bl_frame_decode(out, n, NULL, 0, &g), BL_E_FORMAT);
    /* oversize payload rejected on encode */
    f.payload_len = BL_MAX_PAYLOAD + 1;
    CHECK_EQ_INT(bl_frame_encode(&f, NULL, 0, out, sizeof(out)), BL_E_TOO_LARGE);
    /* tiny out buffer rejected */
    f.payload_len = 0;
    CHECK_EQ_INT(bl_frame_encode(&f, NULL, 0, out, 4), BL_E_OVERFLOW);
}

/* ------------------------------------------------------------- session */

static void run_handshake(bl_session_t* c, bl_session_t* p) {
    prng_t rc = {0xC0FFEE}, rp = {0xBEEF};
    uint8_t psk[BL_KEY_LEN];
    memset(psk, 0x42, sizeof(psk));
    CHECK_EQ_INT(bl_session_init(c, BL_ROLE_CENTRAL, psk, prng, &rc), BL_OK);
    CHECK_EQ_INT(bl_session_init(p, BL_ROLE_PERIPHERAL, psk, prng, &rp), BL_OK);

    uint8_t b1[BL_MAX_MSG], b2[BL_MAX_MSG], b3[BL_MAX_MSG], b4[BL_MAX_MSG];
    size_t l;
    int n = bl_session_start(c, b1, sizeof(b1));
    CHECK(n > 0);
    /* P <- HELLO ; P -> HELLO_ACK */
    CHECK_EQ_INT(bl_session_handshake_rx(p, b1, n, b2, sizeof(b2), &l), BL_OK);
    CHECK(l > 0);
    /* C <- HELLO_ACK ; C -> AUTH */
    CHECK_EQ_INT(bl_session_handshake_rx(c, b2, l, b3, sizeof(b3), &l), BL_OK);
    CHECK(l > 0);
    /* P <- AUTH ; P -> AUTH_OK (established) */
    CHECK_EQ_INT(bl_session_handshake_rx(p, b3, l, b4, sizeof(b4), &l), BL_OK);
    CHECK(bl_session_established(p));
    CHECK(l > 0);
    /* C <- AUTH_OK (established) */
    size_t l2;
    uint8_t b5[BL_MAX_MSG];
    CHECK_EQ_INT(bl_session_handshake_rx(c, b4, l, b5, sizeof(b5), &l2), BL_OK);
    CHECK(bl_session_established(c));
}

static void t_handshake_and_data(void) {
    bl_session_t c, p;
    run_handshake(&c, &p);
    /* keys agree: C tx == P rx and vice versa */
    CHECK(memcmp(c.tx_key, p.rx_key, BL_KEY_LEN) == 0);
    CHECK(memcmp(c.rx_key, p.tx_key, BL_KEY_LEN) == 0);

    /* C seals PING, P opens it */
    uint8_t buf[BL_MAX_MSG];
    const char* msg = "hello-bruce";
    int n = bl_session_seal(
        &c, BL_MSG_PING, 0, (const uint8_t*)msg, (uint16_t)strlen(msg), buf, sizeof(buf));
    CHECK(n > 0);
    bl_frame_t f;
    CHECK_EQ_INT(bl_session_open(&p, buf, n, &f), BL_OK);
    CHECK_EQ_INT(f.type, BL_MSG_PING);
    CHECK(f.payload_len == strlen(msg) && memcmp(f.payload, msg, f.payload_len) == 0);

    bl_session_close(&c);
    bl_session_close(&p);
}

static void t_handshake_wrong_psk_fails(void) {
    prng_t rc = {1}, rp = {2};
    uint8_t psk_c[BL_KEY_LEN], psk_p[BL_KEY_LEN];
    memset(psk_c, 0x11, sizeof(psk_c));
    memset(psk_p, 0x22, sizeof(psk_p)); /* different */
    bl_session_t c, p;
    bl_session_init(&c, BL_ROLE_CENTRAL, psk_c, prng, &rc);
    bl_session_init(&p, BL_ROLE_PERIPHERAL, psk_p, prng, &rp);
    uint8_t b1[BL_MAX_MSG], b2[BL_MAX_MSG], b3[BL_MAX_MSG];
    size_t l;
    int n = bl_session_start(&c, b1, sizeof(b1));
    /* peripheral will produce an ACK (it cannot know yet) */
    CHECK_EQ_INT(bl_session_handshake_rx(&p, b1, n, b2, sizeof(b2), &l), BL_OK);
    /* central verifies confirm_P with its own PSK -> mismatch -> AUTH fail */
    int rc2 = bl_session_handshake_rx(&c, b2, l, b3, sizeof(b3), &l);
    CHECK_EQ_INT(rc2, BL_E_AUTH);
    CHECK_EQ_INT(c.state, BL_SS_FAILED);
    CHECK(c.auth_failures == 1);
}

static void t_replay_rejected(void) {
    bl_session_t c, p;
    run_handshake(&c, &p);
    uint8_t m1[BL_MAX_MSG], m2[BL_MAX_MSG];
    int n1 = bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, m1, sizeof(m1));
    int n2 = bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, m2, sizeof(m2));
    bl_frame_t f;
    CHECK_EQ_INT(bl_session_open(&p, m1, n1, &f), BL_OK);
    CHECK_EQ_INT(bl_session_open(&p, m2, n2, &f), BL_OK);
    /* replaying m1 (lower counter) must be rejected */
    CHECK_EQ_INT(bl_session_open(&p, m1, n1, &f), BL_E_REPLAY);
    /* replaying m2 (equal to highest) must be rejected */
    CHECK_EQ_INT(bl_session_open(&p, m2, n2, &f), BL_E_REPLAY);
}

static void t_cross_key_rejected(void) {
    /* a frame sealed by C must not verify against C's own rx key */
    bl_session_t c, p;
    run_handshake(&c, &p);
    uint8_t m[BL_MAX_MSG];
    int n = bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, m, sizeof(m));
    bl_frame_t f;
    /* open with C (uses c.rx_key = p.tx_key) -> MAC mismatch */
    c.rx_highest = 0;
    CHECK_EQ_INT(bl_session_open(&c, m, n, &f), BL_E_MAC);
}

/* ------------------------------------------------------- fragmentation */

static void frag_roundtrip(size_t msg_len, size_t chunk_cap) {
    uint8_t msg[BL_MAX_MSG];
    for(size_t i = 0; i < msg_len; i++) msg[i] = (uint8_t)(i * 7 + 3);
    bl_reasm_t r;
    bl_reasm_init(&r);
    unsigned i = 0;
    int done = 0;
    const uint8_t* frame = NULL;
    size_t flen = 0;
    while(!done) {
        uint8_t chunk[256];
        size_t clen;
        int rc = bl_frag_chunk(msg, msg_len, chunk_cap, i, chunk, sizeof(chunk), &clen);
        CHECK(rc >= 0);
        done = rc;
        int prc = bl_reasm_push(&r, chunk, clen, &frame, &flen);
        CHECK_EQ_INT(prc, BL_OK);
        i++;
    }
    CHECK_EQ_INT(flen, msg_len);
    CHECK(frame && memcmp(frame, msg, msg_len) == 0);
}

static void t_frag_roundtrip_sizes(void) {
    frag_roundtrip(1, 20);
    frag_roundtrip(19, 20); /* 18 data/chunk */
    frag_roundtrip(200, 244);
    frag_roundtrip(244 - 2, 244); /* exactly one chunk */
    frag_roundtrip(BL_MAX_MSG, 244); /* max frame over BLE MTU */
    frag_roundtrip(BL_MAX_MSG, 23); /* min realistic BLE MTU */
}

static void t_frag_count(void) {
    CHECK_EQ_INT(bl_frag_count(0, 20), 1);
    CHECK_EQ_INT(bl_frag_count(18, 20), 1);
    CHECK_EQ_INT(bl_frag_count(19, 20), 2);
    CHECK_EQ_INT(bl_frag_count(BL_MAX_MSG, 244), (BL_MAX_MSG + 241) / 242);
}

static void t_reasm_lost_fragment(void) {
    /* drop the middle chunk: reassembler must error, not silently join */
    uint8_t msg[500];
    for(int i = 0; i < 500; i++) msg[i] = (uint8_t)i;
    bl_reasm_t r;
    bl_reasm_init(&r);
    const uint8_t* frame;
    size_t flen;
    uint8_t c0[64], c2[64];
    size_t l0, l2;
    bl_frag_chunk(msg, 500, 64, 0, c0, sizeof(c0), &l0);
    bl_frag_chunk(msg, 500, 64, 2, c2, sizeof(c2), &l2); /* skip index 1 */
    CHECK_EQ_INT(bl_reasm_push(&r, c0, l0, &frame, &flen), BL_OK);
    CHECK_EQ_INT(bl_reasm_push(&r, c2, l2, &frame, &flen), BL_E_SEQ);
}

static void t_reasm_missing_first(void) {
    bl_reasm_t r;
    bl_reasm_init(&r);
    const uint8_t* frame;
    size_t flen;
    uint8_t chunk[8] = {0 /*no FIRST*/, 0, 1, 2, 3};
    CHECK_EQ_INT(bl_reasm_push(&r, chunk, 5, &frame, &flen), BL_E_SEQ);
}

/* --------------------------------------------------------------- cobs */

static void cobs_roundtrip(const uint8_t* in, size_t n) {
    uint8_t enc[BL_COBS_MAX_ENCODED(BL_MAX_MSG)];
    uint8_t dec[BL_MAX_MSG];
    int e = bl_cobs_encode(in, n, enc, sizeof(enc));
    CHECK(e > 0);
    CHECK_EQ_INT(enc[e - 1], 0); /* delimiter */
    for(int i = 0; i < e - 1; i++) CHECK(enc[i] != 0); /* no zeros inside */
    int d = bl_cobs_decode(enc, e - 1, dec, sizeof(dec));
    CHECK_EQ_INT(d, (int)n);
    CHECK(memcmp(dec, in, n) == 0);
}

static void t_cobs_roundtrip(void) {
    uint8_t a[] = {0x11, 0x22, 0x00, 0x33};
    cobs_roundtrip(a, sizeof(a));
    uint8_t zeros[10] = {0};
    cobs_roundtrip(zeros, sizeof(zeros));
    /* a full-size frame exercises both embedded zeros and >254 runs */
    uint8_t big[BL_MAX_MSG];
    for(size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i % 256);
    cobs_roundtrip(big, sizeof(big));
    uint8_t norun[300]; /* no zero byte, forces a 0xFF code block */
    for(int i = 0; i < 300; i++) norun[i] = (uint8_t)(1 + (i % 254));
    cobs_roundtrip(norun, sizeof(norun));
}

static void t_cobs_stream(void) {
    /* encode two frames, stream the bytes, recover both */
    uint8_t f1[] = {1, 2, 3, 0, 4};
    uint8_t f2[] = {0, 0, 9};
    uint8_t enc1[64], enc2[64];
    int e1 = bl_cobs_encode(f1, sizeof(f1), enc1, sizeof(enc1));
    int e2 = bl_cobs_encode(f2, sizeof(f2), enc2, sizeof(enc2));
    bl_cobs_stream_t s;
    bl_cobs_stream_init(&s);
    uint8_t out[BL_MAX_MSG];
    size_t ol;
    int got = 0;
    for(int i = 0; i < e1; i++) {
        int rc = bl_cobs_stream_push(&s, enc1[i], out, sizeof(out), &ol);
        if(rc == BL_OK) {
            got++;
            CHECK(ol == sizeof(f1) && memcmp(out, f1, ol) == 0);
        }
    }
    for(int i = 0; i < e2; i++) {
        int rc = bl_cobs_stream_push(&s, enc2[i], out, sizeof(out), &ol);
        if(rc == BL_OK) {
            got++;
            CHECK(ol == sizeof(f2) && memcmp(out, f2, ol) == 0);
        }
    }
    CHECK_EQ_INT(got, 2);
}

static void t_cobs_resync_after_garbage(void) {
    /* garbage bytes then a delimiter then a valid frame must recover */
    uint8_t f1[] = {5, 6, 7};
    uint8_t enc[64];
    int e = bl_cobs_encode(f1, sizeof(f1), enc, sizeof(enc));
    bl_cobs_stream_t s;
    bl_cobs_stream_init(&s);
    uint8_t out[BL_MAX_MSG];
    size_t ol;
    /* feed a stray 0 (empty frame -> SHORT), then junk, then delimiter, then real */
    bl_cobs_stream_push(&s, 0x00, out, sizeof(out), &ol);
    bl_cobs_stream_push(&s, 0xAB, out, sizeof(out), &ol); /* junk code */
    bl_cobs_stream_push(&s, 0x00, out, sizeof(out), &ol); /* ends junk frame */
    int got = 0;
    for(int i = 0; i < e; i++) {
        if(bl_cobs_stream_push(&s, enc[i], out, sizeof(out), &ol) == BL_OK) {
            got++;
            CHECK(ol == sizeof(f1) && memcmp(out, f1, ol) == 0);
        }
    }
    CHECK_EQ_INT(got, 1);
}

void run_protocol_tests(void) {
    RUN(t_frame_roundtrip_unauth);
    RUN(t_frame_mac_roundtrip_and_tamper);
    RUN(t_frame_malformed);
    RUN(t_handshake_and_data);
    RUN(t_handshake_wrong_psk_fails);
    RUN(t_replay_rejected);
    RUN(t_cross_key_rejected);
    RUN(t_frag_roundtrip_sizes);
    RUN(t_frag_count);
    RUN(t_reasm_lost_fragment);
    RUN(t_reasm_missing_first);
    RUN(t_cobs_roundtrip);
    RUN(t_cobs_stream);
    RUN(t_cobs_resync_after_garbage);
}
