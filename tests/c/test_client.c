/* Session-initiator (client) state machine: handshake, single outstanding
 * request, timeouts. SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_client.h"
#include "../../shared/brucelink_core/src/bl_uuid.h"
#include "bl_test.h"

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

typedef struct {
    bl_client_t cl;
    bl_session_t srv; /* responder */
    prng_t rc, rs;
    uint8_t psk[BL_KEY_LEN];
} pair_t;

static void pair_init(pair_t* p, uint8_t client_key, uint8_t server_key) {
    p->rc.s = 0xA1;
    p->rs.s = 0xB2;
    memset(p->psk, client_key, sizeof(p->psk));
    bl_client_init(&p->cl, p->psk, prng, &p->rc);
    uint8_t sk[BL_KEY_LEN];
    memset(sk, server_key, sizeof(sk));
    bl_session_init(&p->srv, BL_ROLE_PERIPHERAL, sk, prng, &p->rs);
}

/* Run the handshake; returns the client's final rc. */
static int pair_handshake(pair_t* p, uint32_t now) {
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t bl = 0, al = 0;
    bl_frame_t m;
    bool have;
    int n = bl_client_link_up(&p->cl, now, a, sizeof(a));
    if(n < 0) return n;
    if(bl_session_handshake_rx(&p->srv, a, (size_t)n, b, sizeof(b), &bl) != BL_OK) return -100;
    int rc = bl_client_rx(&p->cl, b, bl, a, sizeof(a), &al, &m, &have); /* HELLO_ACK -> AUTH */
    if(rc != BL_OK) return rc;
    if(bl_session_handshake_rx(&p->srv, a, al, b, sizeof(b), &bl) != BL_OK) return -101;
    return bl_client_rx(&p->cl, b, bl, a, sizeof(a), &al, &m, &have); /* AUTH_OK */
}

static void t_client_handshake_and_request(void) {
    pair_t p;
    pair_init(&p, 0x42, 0x42);
    CHECK_EQ_INT(p.cl.state, BL_CL_IDLE);
    CHECK_EQ_INT(pair_handshake(&p, 1000), BL_OK);
    CHECK(bl_client_ready(&p.cl));
    CHECK(bl_session_established(&p.srv));

    /* request -> server opens it -> response -> client delivers + frees slot */
    uint8_t req[BL_MAX_MSG], rsp[BL_MAX_MSG], scratch[BL_CLIENT_HS_FRAME_MAX];
    int n = bl_client_request(&p.cl, BL_MSG_PING, (const uint8_t*)"hi", 2, 1100, req, sizeof(req));
    CHECK(n > 0);
    CHECK(p.cl.req_pending);
    bl_frame_t f;
    CHECK_EQ_INT(bl_session_open(&p.srv, req, (size_t)n, &f), BL_OK);
    CHECK(f.tag != 0); /* requests are tagged */
    n = bl_session_seal_tag(
        &p.srv, BL_MSG_PONG, BL_FLAG_RESPONSE, f.tag, f.payload, f.payload_len, rsp, sizeof(rsp));
    size_t rl = 0;
    bool have = false;
    bl_frame_t m;
    CHECK_EQ_INT(bl_client_rx(&p.cl, rsp, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have), BL_OK);
    CHECK(have && m.type == BL_MSG_PONG && m.payload_len == 2);
    CHECK(!p.cl.req_pending);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 999999), BL_CL_EVT_NONE);
}

/* M4.7 / review 2026-11-23: a reply that arrives after its request timed
 * out used to be taken as the answer to the next request (e.g. a late
 * ERROR failing a job that was just started). Fails on the old client,
 * which freed the slot on any response. */
static void t_client_late_response_dropped(void) {
    pair_t p;
    pair_init(&p, 0x42, 0x42);
    CHECK_EQ_INT(pair_handshake(&p, 1000), BL_OK);
    uint8_t req[BL_MAX_MSG], rsp[BL_MAX_MSG], late[BL_MAX_MSG], scratch[BL_CLIENT_HS_FRAME_MAX];
    bl_frame_t f1, f2, m;
    size_t rl = 0;
    bool have = false;

    /* request A, never answered in time */
    int n = bl_client_request(&p.cl, BL_MSG_DEVICE_INFO_REQ, NULL, 0, 2000, req, sizeof(req));
    CHECK(n > 0 && bl_session_open(&p.srv, req, (size_t)n, &f1) == BL_OK);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 2000 + BL_CLIENT_REQ_TIMEOUT_MS), BL_CL_EVT_REQUEST_TIMEOUT);
    const uint8_t err[2] = {BL_ERR_UNKNOWN_CAPABILITY, 0};
    const int ln = bl_session_seal_tag(
        &p.srv, BL_MSG_ERROR, BL_FLAG_RESPONSE | BL_FLAG_ERROR, f1.tag, err, 2, late, sizeof(late));

    /* request B goes out; then A's late answer arrives */
    n = bl_client_request(&p.cl, BL_MSG_JOB_START, (const uint8_t*)"\x10\x00\x00\x00", 4, 20000, req, sizeof(req));
    CHECK(n > 0 && bl_session_open(&p.srv, req, (size_t)n, &f2) == BL_OK);
    CHECK(f2.tag != f1.tag);
    CHECK_EQ_INT(bl_client_rx(&p.cl, late, (size_t)ln, scratch, sizeof(scratch), &rl, &m, &have), BL_OK);
    CHECK(!have); /* not surfaced */
    CHECK(p.cl.req_pending); /* B is still waiting */
    CHECK_EQ_INT(p.cl.stale_responses, 1);

    /* B's own answer is delivered */
    const uint8_t ok[6] = {1, 2, 3, 4, 20, 0};
    n = bl_session_seal_tag(&p.srv, BL_MSG_JOB_ACCEPTED, BL_FLAG_RESPONSE, f2.tag, ok, 6, rsp, sizeof(rsp));
    CHECK_EQ_INT(bl_client_rx(&p.cl, rsp, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have), BL_OK);
    CHECK(have && m.type == BL_MSG_JOB_ACCEPTED && !p.cl.req_pending);

    /* an unsolicited event (not a response) is still surfaced */
    n = bl_session_seal(&p.srv, BL_MSG_STATUS, BL_FLAG_EVENT, NULL, 0, rsp, sizeof(rsp));
    CHECK_EQ_INT(bl_client_rx(&p.cl, rsp, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have), BL_OK);
    CHECK(have && m.type == BL_MSG_STATUS);

    /* tags are never 0, also across the 8-bit wrap */
    for(int i = 0; i < 300; i++) {
        n = bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 30000, req, sizeof(req));
        CHECK(n > 0 && bl_session_open(&p.srv, req, (size_t)n, &f1) == BL_OK && f1.tag != 0);
        n = bl_session_seal_tag(&p.srv, BL_MSG_PONG, BL_FLAG_RESPONSE, f1.tag, NULL, 0, rsp, sizeof(rsp));
        bl_client_rx(&p.cl, rsp, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have);
        if(!have) break;
    }
    CHECK(have && !p.cl.req_pending);
}

static void t_client_hello_fits_small_buffer(void) {
    pair_t p;
    pair_init(&p, 1, 1);
    uint8_t small[BL_CLIENT_HS_FRAME_MAX];
    int n = bl_client_link_up(&p.cl, 0, small, sizeof(small));
    CHECK(n > 0 && n <= (int)BL_CLIENT_HS_FRAME_MAX);
    /* a buffer that is too small is reported, not overrun */
    uint8_t tiny[16];
    CHECK_EQ_INT(bl_client_link_up(&p.cl, 0, tiny, sizeof(tiny)), BL_E_OVERFLOW);
    CHECK_EQ_INT(p.cl.state, BL_CL_FAILED);
}

static void t_client_wrong_key_fails(void) {
    pair_t p;
    pair_init(&p, 0x11, 0x22);
    CHECK_EQ_INT(pair_handshake(&p, 0), BL_E_AUTH);
    CHECK_EQ_INT(p.cl.state, BL_CL_FAILED);
    CHECK_EQ_INT(p.cl.last_error, BL_E_AUTH);
    /* no requests on a failed link */
    uint8_t out[BL_MAX_MSG];
    CHECK_EQ_INT(bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 0, out, sizeof(out)), BL_E_STATE);
    /* a failed link does not also report a timeout later */
    CHECK_EQ_INT(bl_client_tick(&p.cl, 100000), BL_CL_EVT_NONE);
}

static void t_client_handshake_timeout_once(void) {
    pair_t p;
    pair_init(&p, 3, 3);
    uint8_t a[BL_MAX_MSG];
    CHECK(bl_client_link_up(&p.cl, 5000, a, sizeof(a)) > 0); /* peer never answers */
    CHECK_EQ_INT(bl_client_tick(&p.cl, 5000 + BL_CLIENT_HS_TIMEOUT_MS - 1), BL_CL_EVT_NONE);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 5000 + BL_CLIENT_HS_TIMEOUT_MS), BL_CL_EVT_HANDSHAKE_TIMEOUT);
    CHECK_EQ_INT(p.cl.state, BL_CL_FAILED);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 5000 + 10 * BL_CLIENT_HS_TIMEOUT_MS), BL_CL_EVT_NONE);
    /* a new link recovers */
    pair_init(&p, 3, 3);
    CHECK_EQ_INT(pair_handshake(&p, 20000), BL_OK);
    CHECK(bl_client_ready(&p.cl));
}

static void t_client_handshake_timeout_wraps(void) {
    pair_t p;
    pair_init(&p, 3, 3);
    uint8_t a[BL_MAX_MSG];
    /* The deadline lies across the tick rollover. Expressed relative to the
     * timeout (it was 3 s when this was written with literal tick values). */
    const uint32_t start = 0xFFFFFC00u; /* 1024 ms before rollover */
    CHECK(BL_CLIENT_HS_TIMEOUT_MS > 1024u + 256u); /* so both probes are past the wrap */
    bl_client_link_up(&p.cl, start, a, sizeof(a));
    CHECK_EQ_INT(bl_client_tick(&p.cl, 0x00000100u), BL_CL_EVT_NONE); /* +1280 ms, wrapped */
    CHECK_EQ_INT(bl_client_tick(&p.cl, start + BL_CLIENT_HS_TIMEOUT_MS - 1u), BL_CL_EVT_NONE);
    CHECK_EQ_INT(
        bl_client_tick(&p.cl, start + BL_CLIENT_HS_TIMEOUT_MS), BL_CL_EVT_HANDSHAKE_TIMEOUT);
}

static void t_client_request_timeout_and_busy(void) {
    pair_t p;
    pair_init(&p, 7, 7);
    CHECK_EQ_INT(pair_handshake(&p, 0), BL_OK);
    uint8_t out[BL_MAX_MSG];
    CHECK(bl_client_request(&p.cl, BL_MSG_STATUS_REQ, NULL, 0, 1000, out, sizeof(out)) > 0);
    /* one request at a time */
    CHECK_EQ_INT(bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 1001, out, sizeof(out)), BL_E_BUSY);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 1000 + BL_CLIENT_REQ_TIMEOUT_MS - 1), BL_CL_EVT_NONE);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 1000 + BL_CLIENT_REQ_TIMEOUT_MS), BL_CL_EVT_REQUEST_TIMEOUT);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 1000 + 2 * BL_CLIENT_REQ_TIMEOUT_MS), BL_CL_EVT_NONE);
    /* the session survives a request timeout and the slot is free again */
    CHECK(bl_client_ready(&p.cl));
    CHECK(bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 9000, out, sizeof(out)) > 0);
}

static void t_client_event_frames_keep_request_pending(void) {
    pair_t p;
    pair_init(&p, 9, 9);
    CHECK_EQ_INT(pair_handshake(&p, 0), BL_OK);
    uint8_t out[BL_MAX_MSG], ev[BL_MAX_MSG], scratch[BL_CLIENT_HS_FRAME_MAX];
    bl_client_request(&p.cl, BL_MSG_STATUS_REQ, NULL, 0, 0, out, sizeof(out));
    /* an unsolicited event is delivered but does not complete the request */
    int n = bl_session_seal(&p.srv, BL_MSG_STATUS, BL_FLAG_EVENT, (const uint8_t*)"x", 1, ev, sizeof(ev));
    size_t rl;
    bool have;
    bl_frame_t m;
    CHECK_EQ_INT(bl_client_rx(&p.cl, ev, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have), BL_OK);
    CHECK(have);
    CHECK(p.cl.req_pending);
    /* a replayed frame is rejected and delivers nothing */
    CHECK_EQ_INT(bl_client_rx(&p.cl, ev, (size_t)n, scratch, sizeof(scratch), &rl, &m, &have), BL_E_REPLAY);
    CHECK(!have);
}

static void t_client_link_down_wipes_and_resets(void) {
    pair_t p;
    pair_init(&p, 5, 5);
    CHECK_EQ_INT(pair_handshake(&p, 0), BL_OK);
    uint8_t out[BL_MAX_MSG];
    bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 0, out, sizeof(out));
    bl_client_link_down(&p.cl);
    CHECK_EQ_INT(p.cl.state, BL_CL_IDLE);
    CHECK(!p.cl.req_pending);
    CHECK(!p.cl.session.keys_ready);
    uint8_t zero[BL_KEY_LEN] = {0};
    CHECK(memcmp(p.cl.session.tx_key, zero, BL_KEY_LEN) == 0);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 999999), BL_CL_EVT_NONE);
    CHECK_EQ_INT(bl_client_request(&p.cl, BL_MSG_PING, NULL, 0, 0, out, sizeof(out)), BL_E_STATE);
    /* the provisioned key survives for the next link */
    uint8_t want[BL_KEY_LEN];
    memset(want, 5, sizeof(want));
    CHECK(memcmp(p.cl.psk, want, BL_KEY_LEN) == 0);
}

static void t_client_frames_before_link_ignored(void) {
    bl_client_t c;
    prng_t r = {1};
    uint8_t k[BL_KEY_LEN] = {0};
    bl_client_init(&c, k, prng, &r);
    uint8_t junk[40] = {0}, scratch[BL_CLIENT_HS_FRAME_MAX];
    size_t rl;
    bool have;
    bl_frame_t m;
    CHECK_EQ_INT(bl_client_rx(&c, junk, sizeof(junk), scratch, sizeof(scratch), &rl, &m, &have), BL_E_STATE);
    CHECK(!have && rl == 0);
}

/* The Flipper's little-endian UUID bytes must be the StickS3's UUID strings
 * reversed, or the two devices would look for different services. */
static void uuid_str_to_le(const char* str, uint8_t out[16]) {
    uint8_t be[16];
    int n = 0;
    for(const char* p = str; *p && n < 16; p++) {
        if(*p == '-') continue;
        unsigned v;
        sscanf(p, "%2x", &v);
        be[n++] = (uint8_t)v;
        p++;
    }
    for(int i = 0; i < 16; i++) out[i] = be[15 - i];
}

static void t_uuid_forms_agree(void) {
    const uint8_t svc[16] = BL_UUID_SERVICE_LE, rx[16] = BL_UUID_RX_LE, tx[16] = BL_UUID_TX_LE;
    uint8_t want[16];
    CHECK_EQ_INT(strlen(BL_UUID_SERVICE_STR), 36);
    uuid_str_to_le(BL_UUID_SERVICE_STR, want);
    CHECK(memcmp(svc, want, 16) == 0);
    uuid_str_to_le(BL_UUID_RX_STR, want);
    CHECK(memcmp(rx, want, 16) == 0);
    uuid_str_to_le(BL_UUID_TX_STR, want);
    CHECK(memcmp(tx, want, 16) == 0);
    CHECK(memcmp(svc, rx, 16) != 0 && memcmp(rx, tx, 16) != 0);
}

/* Review P2: any handshake receive error used to be terminal, so one stray
 * or garbled frame left the Flipper stuck until the StickS3's 10 s deadline
 * and 30 s cooldown. Only a failure of the session itself is terminal. */
static void t_client_stray_handshake_frame_ignored(void) {
    pair_t p;
    pair_init(&p, 7, 7);
    uint8_t hello[BL_MAX_MSG], ack[BL_MAX_MSG], out[BL_MAX_MSG];
    size_t ackl = 0, ol = 0;
    bl_frame_t m;
    bool have = true;
    int n = bl_client_link_up(&p.cl, 100, hello, sizeof(hello));
    CHECK(n > 0);

    /* (a) noise */
    uint8_t junk[40];
    memset(junk, 0xA5, sizeof(junk));
    CHECK(bl_client_rx(&p.cl, junk, sizeof(junk), out, sizeof(out), &ol, &m, &have) != BL_OK);
    CHECK_EQ_INT(p.cl.state, BL_CL_HANDSHAKING);
    CHECK_EQ_INT(ol, 0);
    CHECK(!have);
    /* (b) a well-formed frame of the wrong kind (our own HELLO, echoed) */
    CHECK(bl_client_rx(&p.cl, hello, (size_t)n, out, sizeof(out), &ol, &m, &have) != BL_OK);
    CHECK_EQ_INT(p.cl.state, BL_CL_HANDSHAKING);
    CHECK_EQ_INT(ol, 0);
    CHECK_EQ_INT(p.cl.last_error, BL_OK);

    /* the real handshake still completes */
    CHECK_EQ_INT(bl_session_handshake_rx(&p.srv, hello, (size_t)n, ack, sizeof(ack), &ackl), BL_OK);
    CHECK_EQ_INT(bl_client_rx(&p.cl, ack, ackl, out, sizeof(out), &ol, &m, &have), BL_OK);
    CHECK(ol > 0);
    CHECK_EQ_INT(bl_session_handshake_rx(&p.srv, out, ol, ack, sizeof(ack), &ackl), BL_OK);
    CHECK_EQ_INT(bl_client_rx(&p.cl, ack, ackl, out, sizeof(out), &ol, &m, &have), BL_OK);
    CHECK(bl_client_ready(&p.cl));

    /* and noise alone still ends in the timeout, not a hang */
    pair_init(&p, 7, 7);
    CHECK(bl_client_link_up(&p.cl, 100, hello, sizeof(hello)) > 0);
    (void)bl_client_rx(&p.cl, junk, sizeof(junk), out, sizeof(out), &ol, &m, &have);
    CHECK_EQ_INT(bl_client_tick(&p.cl, 100 + BL_CLIENT_HS_TIMEOUT_MS), BL_CL_EVT_HANDSHAKE_TIMEOUT);
}

/* Review P2: the StickS3 sized its chunks as MTU - 3 with no upper bound,
 * while the Flipper drops anything over 244. One rule for both ends. */
static void t_ble_chunk_for_mtu(void) {
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(0), BL_BLE_CHUNK_DEFAULT);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(22), BL_BLE_CHUNK_DEFAULT);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(23), 20);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(100), 97);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(247), 244);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(248), BL_BLE_CHUNK_MAX);
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(414), BL_BLE_CHUNK_MAX); /* Flipper's stack maximum */
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(517), BL_BLE_CHUNK_MAX); /* NimBLE's maximum */
    CHECK_EQ_INT(bl_ble_chunk_for_mtu(0xFFFF), BL_BLE_CHUNK_MAX);
    /* the handshake must not be starved by its own computation time, and
     * must give up before the StickS3's 10 s deadline */
    CHECK(BL_CLIENT_HS_TIMEOUT_MS >= 5000u && BL_CLIENT_HS_TIMEOUT_MS < 10000u);
}

/* Review 2026-11-23: the version range HELLO offered was not in the
 * transcript, so rewriting it on the way (a downgrade) went unnoticed. A
 * HELLO with a changed range must now fail the confirm tags. Fails on the
 * old transcript (only proto_sel was hashed). */
static void t_session_hello_range_is_authenticated(void) {
    uint8_t psk[BL_KEY_LEN];
    memset(psk, 0x33, sizeof(psk));
    prng_t ra = {0x11}, rb = {0x22};
    bl_session_t c, s;
    bl_session_init(&c, BL_ROLE_CENTRAL, psk, prng, &ra);
    bl_session_init(&s, BL_ROLE_PERIPHERAL, psk, prng, &rb);
    uint8_t hello[BL_MAX_MSG], ack[BL_MAX_MSG], auth[BL_MAX_MSG];
    size_t al = 0, ul = 0;
    const int n = bl_session_start(&c, hello, sizeof(hello));
    CHECK(n > 0);
    /* an attacker widens/narrows the offered range; the selection (1) is
     * unchanged, so only the transcript can notice */
    hello[BL_HDR_LEN + 0] = 0;
    hello[BL_HDR_LEN + 1] = 9;
    CHECK_EQ_INT(bl_session_handshake_rx(&s, hello, (size_t)n, ack, sizeof(ack), &al), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, ack, al, auth, sizeof(auth), &ul), BL_E_AUTH);
    CHECK_EQ_INT(c.state, BL_SS_FAILED);
}

/* Review 2026-11-23: keys exist from HELLO on, and open/seal only checked
 * that; a responder waiting for AUTH (or failed) still opened frames, and
 * a failed session kept its secrets until closed. */
static void t_session_strict_states_and_wipe(void) {
    uint8_t psk[BL_KEY_LEN];
    memset(psk, 0x44, sizeof(psk));
    prng_t ra = {0x31}, rb = {0x42};
    bl_session_t c, s;
    bl_session_init(&c, BL_ROLE_CENTRAL, psk, prng, &ra);
    bl_session_init(&s, BL_ROLE_PERIPHERAL, psk, prng, &rb);
    uint8_t hello[BL_MAX_MSG], ack[BL_MAX_MSG], auth[BL_MAX_MSG], fr[BL_MAX_MSG];
    size_t al = 0, ul = 0;
    const int n = bl_session_start(&c, hello, sizeof(hello));
    CHECK_EQ_INT(bl_session_handshake_rx(&s, hello, (size_t)n, ack, sizeof(ack), &al), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, ack, al, auth, sizeof(auth), &ul), BL_OK);
    /* the central has keys but is not established: it may not seal */
    CHECK_EQ_INT(bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, fr, sizeof(fr)), BL_E_STATE);
    /* the responder waits for AUTH: a sealed frame is not opened */
    bl_frame_t f;
    CHECK_EQ_INT(bl_session_open(&s, auth, ul, &f), BL_E_STATE);
    /* a wrong AUTH fails the responder and wipes everything */
    auth[BL_HDR_LEN] ^= 1;
    CHECK_EQ_INT(bl_session_handshake_rx(&s, auth, ul, fr, sizeof(fr), &al), BL_E_AUTH);
    CHECK_EQ_INT(s.state, BL_SS_FAILED);
    CHECK(!s.keys_ready);
    static const uint8_t zero[BL_KEY_LEN];
    CHECK(memcmp(s.psk, zero, BL_KEY_LEN) == 0);
    CHECK(memcmp(s.tx_key, zero, BL_KEY_LEN) == 0);
    CHECK(memcmp(s.rx_key, zero, BL_KEY_LEN) == 0);
    CHECK(memcmp(s.eph_sk, zero, 32) == 0);
    CHECK_EQ_INT(bl_session_open(&s, auth, ul, &f), BL_E_STATE);
}

/* Review 2026-11-23: in WAIT_AUTH_OK any well-formed frame that failed to
 * open was terminal, so a repeated HELLO_ACK ended the handshake. Only an
 * AUTH_OK may end it now; other frames are ignored as in the other states. */
static void t_session_stray_frame_while_waiting_auth_ok(void) {
    uint8_t psk[BL_KEY_LEN];
    memset(psk, 0x55, sizeof(psk));
    prng_t ra = {0x61}, rb = {0x72};
    bl_session_t c, s;
    bl_session_init(&c, BL_ROLE_CENTRAL, psk, prng, &ra);
    bl_session_init(&s, BL_ROLE_PERIPHERAL, psk, prng, &rb);
    uint8_t hello[BL_MAX_MSG], ack[BL_MAX_MSG], auth[BL_MAX_MSG], ok[BL_MAX_MSG], fr[BL_MAX_MSG];
    size_t al = 0, ul = 0, ol = 0, fl = 0;
    const int n = bl_session_start(&c, hello, sizeof(hello));
    bl_session_handshake_rx(&s, hello, (size_t)n, ack, sizeof(ack), &al);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, ack, al, auth, sizeof(auth), &ul), BL_OK);
    CHECK_EQ_INT(c.state, BL_SS_WAIT_AUTH_OK);
    /* the HELLO_ACK again (a duplicated notification) */
    CHECK_EQ_INT(bl_session_handshake_rx(&c, ack, al, fr, sizeof(fr), &fl), BL_E_STATE);
    CHECK_EQ_INT(c.state, BL_SS_WAIT_AUTH_OK);
    CHECK_EQ_INT(c.auth_failures, 0);
    /* the real AUTH_OK still completes it */
    CHECK_EQ_INT(bl_session_handshake_rx(&s, auth, ul, ok, sizeof(ok), &ol), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, ok, ol, fr, sizeof(fr), &fl), BL_OK);
    CHECK(bl_session_established(&c));
    /* a forged AUTH_OK (bad MAC) is still fatal */
    bl_session_t c2, s2;
    prng_t rc2 = {0x81}, rs2 = {0x92};
    bl_session_init(&c2, BL_ROLE_CENTRAL, psk, prng, &rc2);
    bl_session_init(&s2, BL_ROLE_PERIPHERAL, psk, prng, &rs2);
    const int n2 = bl_session_start(&c2, hello, sizeof(hello));
    bl_session_handshake_rx(&s2, hello, (size_t)n2, ack, sizeof(ack), &al);
    bl_session_handshake_rx(&c2, ack, al, auth, sizeof(auth), &ul);
    bl_session_handshake_rx(&s2, auth, ul, ok, sizeof(ok), &ol);
    ok[ol - 1] ^= 1;
    CHECK_EQ_INT(bl_session_handshake_rx(&c2, ok, ol, fr, sizeof(fr), &fl), BL_E_MAC);
    CHECK_EQ_INT(c2.state, BL_SS_FAILED);
}

static void establish(bl_session_t* c, bl_session_t* s, uint8_t fill) {
    uint8_t psk[BL_KEY_LEN];
    memset(psk, fill, sizeof(psk));
    prng_t ra = {fill}, rb = {(uint32_t)fill * 7u};
    bl_session_init(c, BL_ROLE_CENTRAL, psk, prng, &ra);
    bl_session_init(s, BL_ROLE_PERIPHERAL, psk, prng, &rb);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    const int n = bl_session_start(c, a, sizeof(a));
    bl_session_handshake_rx(s, a, (size_t)n, b, sizeof(b), &bl);
    bl_session_handshake_rx(c, b, bl, a, sizeof(a), &al);
    bl_session_handshake_rx(s, a, al, b, sizeof(b), &bl);
    bl_session_handshake_rx(c, b, bl, a, sizeof(a), &al);
}

/* Review 2026-11-23: no test flipped header fields of a sealed frame, and
 * none ran the send counter to its end. */
static void t_session_header_tamper_and_counter_end(void) {
    bl_session_t c, s;
    establish(&c, &s, 0x66);
    CHECK(bl_session_established(&c) && bl_session_established(&s));
    uint8_t fr[BL_MAX_MSG], bad[BL_MAX_MSG];
    static const uint8_t pl[4] = {9, 8, 7, 6};
    const int n = bl_session_seal_tag(&c, BL_MSG_PING, 0, 0x21, pl, sizeof(pl), fr, sizeof(fr));
    CHECK(n > 0);
    bl_frame_t f;
    /* type, flags, tag, every seq byte, a payload byte: none opens, and a
     * failed open does not move the replay window */
    static const size_t offs[] = {3, 4, 5, 6, 7, 8, 9, 12};
    for(size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        memcpy(bad, fr, (size_t)n);
        bad[offs[i]] ^= 0x01;
        CHECK(bl_session_open(&s, bad, (size_t)n, &f) != BL_OK);
    }
    CHECK_EQ_INT(bl_session_open(&s, fr, (size_t)n, &f), BL_OK);
    CHECK_EQ_INT(f.tag, 0x21);
    CHECK_EQ_INT(bl_session_open(&s, fr, (size_t)n, &f), BL_E_REPLAY);
    /* the last counter value is never used: sealing stops before it */
    c.tx_counter = 0xFFFFFFFEu;
    CHECK(bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, fr, sizeof(fr)) > 0);
    CHECK_EQ_INT(bl_session_seal(&c, BL_MSG_PING, 0, NULL, 0, fr, sizeof(fr)), BL_E_STATE);
    CHECK_EQ_INT((long)c.tx_counter, (long)0xFFFFFFFFu);
}

void run_client_tests(void) {
    RUN(t_session_header_tamper_and_counter_end);
    RUN(t_client_late_response_dropped);
    RUN(t_session_hello_range_is_authenticated);
    RUN(t_session_strict_states_and_wipe);
    RUN(t_session_stray_frame_while_waiting_auth_ok);
    RUN(t_client_stray_handshake_frame_ignored);
    RUN(t_ble_chunk_for_mtu);
    RUN(t_uuid_forms_agree);
    RUN(t_client_handshake_and_request);
    RUN(t_client_hello_fits_small_buffer);
    RUN(t_client_wrong_key_fails);
    RUN(t_client_handshake_timeout_once);
    RUN(t_client_handshake_timeout_wraps);
    RUN(t_client_request_timeout_and_busy);
    RUN(t_client_event_frames_keep_request_pending);
    RUN(t_client_link_down_wipes_and_resets);
    RUN(t_client_frames_before_link_ignored);
}
