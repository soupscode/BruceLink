/* Pairing: key agreement with code comparison (bl_pair, ADR 0005).
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_crypto.h"
#include "../../shared/brucelink_core/src/bl_pair.h"
#include "bl_test.h"

typedef struct {
    uint32_t s;
} prng_t;
static void prng(void* ctx, uint8_t* buf, size_t len) {
    prng_t* p = (prng_t*)ctx;
    for(size_t i = 0; i < len; i++) {
        p->s = p->s * 1103515245u + 12345u;
        buf[i] = (uint8_t)(p->s >> 16);
    }
}

typedef struct {
    bl_pair_t i, r; /* initiator = Flipper, responder = StickS3 */
    prng_t ri, rr;
    uint8_t buf[BL_MAX_MSG];
    size_t len;
} duo_t;

static void duo_init(duo_t* d, uint32_t seed_i, uint32_t seed_r) {
    d->ri.s = seed_i;
    d->rr.s = seed_r;
    CHECK_EQ_INT(bl_pair_init(&d->i, BL_PAIR_INITIATOR, "Flipper Aster", prng, &d->ri, 1000), BL_OK);
    CHECK_EQ_INT(bl_pair_init(&d->r, BL_PAIR_RESPONDER, "StickS3", prng, &d->rr, 5000), BL_OK);
}

/* COMMIT -> KEY -> REVEAL: both sides end in COMPARE. */
static void duo_exchange(duo_t* d) {
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    int n = bl_pair_start(&d->i, a, sizeof(a));
    CHECK(n > 0 && (size_t)n <= BL_PAIR_FRAME_MAX);
    CHECK_EQ_INT(bl_pair_rx(&d->r, a, (size_t)n, b, sizeof(b), &bl), BL_OK); /* COMMIT */
    CHECK(bl > 0 && bl <= BL_PAIR_FRAME_MAX);
    CHECK_EQ_INT(bl_pair_rx(&d->i, b, bl, a, sizeof(a), &al), BL_OK); /* KEY */
    CHECK(al > 0 && al <= BL_PAIR_FRAME_MAX);
    CHECK_EQ_INT(bl_pair_rx(&d->r, a, al, b, sizeof(b), &bl), BL_OK); /* REVEAL */
    CHECK_EQ_INT(bl, 0);
    CHECK_EQ_INT(bl_pair_state(&d->i), BL_PS_COMPARE);
    CHECK_EQ_INT(bl_pair_state(&d->r), BL_PS_COMPARE);
}

/* Deliver `from`'s accept/reject decision to `to`. */
static int duo_answer(bl_pair_t* from, bl_pair_t* to, bool accept) {
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    int rc = bl_pair_accept(from, accept, a, sizeof(a), &al);
    if(rc != BL_OK) return rc;
    CHECK(al > 0 && al <= BL_PAIR_FRAME_MAX);
    rc = bl_pair_rx(to, a, al, b, sizeof(b), &bl);
    CHECK_EQ_INT(bl, 0);
    return rc;
}

static bool all_zero(const uint8_t* b, size_t n) {
    for(size_t i = 0; i < n; i++)
        if(b[i]) return false;
    return true;
}

static void t_pair_happy_path(void) {
    duo_t d;
    duo_init(&d, 0x1111, 0x2222);
    duo_exchange(&d);
    uint32_t ci = 0, cr = 1;
    CHECK(bl_pair_code(&d.i, &ci));
    CHECK(bl_pair_code(&d.r, &cr));
    CHECK_EQ_INT(ci, cr);
    CHECK(ci < BL_PAIR_CODE_MOD);
    /* names crossed over */
    CHECK(strcmp(bl_pair_peer_name(&d.i), "StickS3") == 0);
    CHECK(strcmp(bl_pair_peer_name(&d.r), "Flipper Aster") == 0);
    /* the private key is used once and wiped */
    CHECK(all_zero(d.i.sk, sizeof(d.i.sk)));
    CHECK(all_zero(d.r.sk, sizeof(d.r.sk)));
    /* no result before both accept */
    uint8_t ki[BL_KEY_LEN], kr[BL_KEY_LEN], idi[2], idr[2];
    CHECK_EQ_INT(bl_pair_result(&d.i, ki, idi), BL_E_STATE);

    CHECK_EQ_INT(duo_answer(&d.i, &d.r, true), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_WAIT_PEER);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_COMPARE); /* its user has not answered */
    CHECK_EQ_INT(duo_answer(&d.r, &d.i, true), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_DONE);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_DONE);

    CHECK_EQ_INT(bl_pair_result(&d.i, ki, idi), BL_OK);
    CHECK_EQ_INT(bl_pair_result(&d.r, kr, idr), BL_OK);
    CHECK(memcmp(ki, kr, BL_KEY_LEN) == 0);
    CHECK(memcmp(idi, idr, 2) == 0);
    CHECK(!all_zero(ki, BL_KEY_LEN));
    uint8_t id2[2];
    bl_pair_key_id(ki, id2);
    CHECK(memcmp(id2, idi, 2) == 0);

    /* The new key authenticates a normal session. */
    bl_session_t c, p;
    prng_t sc = {7}, sp = {9};
    CHECK_EQ_INT(bl_session_init(&c, BL_ROLE_CENTRAL, ki, prng, &sc), BL_OK);
    CHECK_EQ_INT(bl_session_init(&p, BL_ROLE_PERIPHERAL, kr, prng, &sp), BL_OK);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    int n = bl_session_start(&c, a, sizeof(a));
    CHECK(n > 0);
    CHECK_EQ_INT(bl_session_handshake_rx(&p, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, b, bl, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&p, a, al, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_session_handshake_rx(&c, b, bl, a, sizeof(a), &al), BL_OK);
    CHECK(bl_session_established(&c) && bl_session_established(&p));

    /* Different runs give different keys and (almost always) codes. */
    duo_t e;
    duo_init(&e, 0x3333, 0x4444);
    duo_exchange(&e);
    CHECK(memcmp(e.i.psk, ki, BL_KEY_LEN) != 0);
}

/* The responder's user answers first: its confirm reaches the initiator
 * while the initiator is still showing the code. */
static void t_pair_peer_confirms_first(void) {
    duo_t d;
    duo_init(&d, 0x51, 0x52);
    duo_exchange(&d);
    CHECK_EQ_INT(duo_answer(&d.r, &d.i, true), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_WAIT_PEER);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_COMPARE); /* still asks its user */
    CHECK(d.i.peer_confirmed);
    CHECK_EQ_INT(duo_answer(&d.i, &d.r, true), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_DONE);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_DONE);
}

static void t_pair_rejected(void) {
    duo_t d;
    duo_init(&d, 0x61, 0x62);
    duo_exchange(&d);
    /* the StickS3 user says the codes differ */
    CHECK_EQ_INT(duo_answer(&d.r, &d.i, false), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_FAILED);
    CHECK_EQ_INT(d.r.fail, BL_PF_REJECTED);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_FAILED);
    CHECK_EQ_INT(d.i.fail, BL_PF_PEER_REJECTED);
    CHECK_EQ_INT(d.i.peer_reason, BL_PAIR_ABORT_REJECTED);
    uint8_t k[BL_KEY_LEN], id[2];
    CHECK_EQ_INT(bl_pair_result(&d.i, k, id), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_result(&d.r, k, id), BL_E_STATE);
    /* the failed attempt's key is gone */
    CHECK(all_zero(d.i.psk, sizeof(d.i.psk)));
    CHECK(all_zero(d.r.psk, sizeof(d.r.psk)));
    /* an accepted side whose peer then rejects also ends with nothing */
    duo_t e;
    duo_init(&e, 0x63, 0x64);
    duo_exchange(&e);
    CHECK_EQ_INT(duo_answer(&e.i, &e.r, true), BL_OK);
    CHECK_EQ_INT(duo_answer(&e.r, &e.i, false), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&e.i), BL_PS_FAILED);
    CHECK_EQ_INT(bl_pair_result(&e.i, k, id), BL_E_STATE);
}

/* A man in the middle runs the protocol with each side using its own
 * keys. The two sides compute different codes; if both users accepted
 * anyway, each confirm tag fails on the other side, because the two legs
 * hold different keys. */
static void t_pair_man_in_the_middle(void) {
    bl_pair_t fl, st, m_to_st, m_to_fl;
    prng_t r1 = {0x71}, r2 = {0x72}, r3 = {0x73}, r4 = {0x74};
    CHECK_EQ_INT(bl_pair_init(&fl, BL_PAIR_INITIATOR, "Flipper", prng, &r1, 0), BL_OK);
    CHECK_EQ_INT(bl_pair_init(&st, BL_PAIR_RESPONDER, "StickS3", prng, &r2, 0), BL_OK);
    /* the attacker poses as a responder to the Flipper and an initiator
     * to the StickS3, with the victims' names */
    CHECK_EQ_INT(bl_pair_init(&m_to_fl, BL_PAIR_RESPONDER, "StickS3", prng, &r3, 0), BL_OK);
    CHECK_EQ_INT(bl_pair_init(&m_to_st, BL_PAIR_INITIATOR, "Flipper", prng, &r4, 0), BL_OK);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;

    /* leg Flipper <-> attacker */
    int n = bl_pair_start(&fl, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&m_to_fl, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&fl, b, bl, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&m_to_fl, a, al, b, sizeof(b), &bl), BL_OK);
    /* leg attacker <-> StickS3 */
    n = bl_pair_start(&m_to_st, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&st, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&m_to_st, b, bl, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&st, a, al, b, sizeof(b), &bl), BL_OK);

    uint32_t c_fl = 0, c_st = 0;
    CHECK(bl_pair_code(&fl, &c_fl));
    CHECK(bl_pair_code(&st, &c_st));
    CHECK(c_fl != c_st); /* what the users would see: different numbers */

    /* both users wrongly accept; the attacker relays the confirms */
    CHECK_EQ_INT(bl_pair_accept(&fl, true, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_accept(&st, true, b, sizeof(b), &bl), BL_OK);
    uint8_t junk[BL_MAX_MSG];
    size_t jl = 0;
    CHECK_EQ_INT(bl_pair_rx(&st, a, al, junk, sizeof(junk), &jl), BL_E_AUTH);
    CHECK_EQ_INT(bl_pair_rx(&fl, b, bl, junk, sizeof(junk), &jl), BL_E_AUTH);
    CHECK_EQ_INT(fl.fail, BL_PF_MISMATCH);
    CHECK_EQ_INT(st.fail, BL_PF_MISMATCH);
    uint8_t k[BL_KEY_LEN], id[2];
    CHECK_EQ_INT(bl_pair_result(&fl, k, id), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_result(&st, k, id), BL_E_STATE);
}

/* Changing anything in flight changes the code. */
static void t_pair_tampering_changes_the_code(void) {
    for(int which = 0; which < 3; which++) {
        duo_t d;
        duo_init(&d, 0x81, 0x82);
        uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
        size_t al = 0, bl = 0;
        int n = bl_pair_start(&d.i, a, sizeof(a));
        if(which == 0) a[BL_HDR_LEN + 34] ^= 0x01; /* a letter of the Flipper's name */
        CHECK_EQ_INT(bl_pair_rx(&d.r, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
        if(which == 1) b[BL_HDR_LEN + 1 + 32 + 3] ^= 0x80; /* responder nonce */
        if(which == 2) b[BL_HDR_LEN + 5] ^= 0x04; /* responder public key */
        CHECK_EQ_INT(bl_pair_rx(&d.i, b, bl, a, sizeof(a), &al), BL_OK);
        CHECK_EQ_INT(bl_pair_rx(&d.r, a, al, b, sizeof(b), &bl), BL_OK);
        uint32_t ci = 0, cr = 0;
        CHECK(bl_pair_code(&d.i, &ci) && bl_pair_code(&d.r, &cr));
        CHECK(ci != cr);
        /* and even accepted, the keys never agree */
        CHECK_EQ_INT(duo_answer(&d.i, &d.r, true), BL_E_AUTH);
        CHECK_EQ_INT(d.r.fail, BL_PF_MISMATCH);
    }
}

/* The initiator's revealed key must be the one it committed to. */
static void t_pair_commitment_enforced(void) {
    duo_t d;
    duo_init(&d, 0x91, 0x92);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    int n = bl_pair_start(&d.i, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&d.i, b, bl, a, sizeof(a), &al), BL_OK);
    a[BL_HDR_LEN + 7] ^= 0x10; /* swap in a different initiator key */
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, al, b, sizeof(b), &bl), BL_E_AUTH);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_FAILED);
    CHECK_EQ_INT(d.r.fail, BL_PF_MISMATCH);
    uint32_t c;
    CHECK(!bl_pair_code(&d.r, &c)); /* no code is ever shown */
    /* the initiator is told */
    CHECK(bl > 0);
    size_t xl = 0;
    CHECK_EQ_INT(bl_pair_rx(&d.i, b, bl, a, sizeof(a), &xl), BL_OK);
    CHECK_EQ_INT(d.i.fail, BL_PF_PEER_REJECTED);
    CHECK_EQ_INT(d.i.peer_reason, BL_PAIR_ABORT_MISMATCH);
}

/* A forged confirm tag fails the attempt even after the local accept. */
static void t_pair_bad_confirm(void) {
    duo_t d;
    duo_init(&d, 0xA1, 0xA2);
    duo_exchange(&d);
    CHECK_EQ_INT(duo_answer(&d.i, &d.r, true), BL_OK); /* r has i's confirm */
    uint8_t a[BL_MAX_MSG], junk[BL_MAX_MSG];
    size_t al = 0, jl = 0;
    CHECK_EQ_INT(bl_pair_accept(&d.r, true, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_DONE);
    a[BL_HDR_LEN + 9] ^= 0x01;
    CHECK_EQ_INT(bl_pair_rx(&d.i, a, al, junk, sizeof(junk), &jl), BL_E_AUTH);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_FAILED);
    CHECK_EQ_INT(d.i.fail, BL_PF_MISMATCH);
    CHECK(all_zero(d.i.psk, sizeof(d.i.psk)));
}

/* Frames that are not the next step change nothing. */
static void t_pair_ignores_out_of_order_and_garbage(void) {
    duo_t d;
    duo_init(&d, 0xB1, 0xB2);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;

    /* a session HELLO reaching a responder in pairing mode: a Flipper that
     * is not pairing. Reported, not fatal. */
    bl_session_t c;
    prng_t sc = {3};
    uint8_t k[BL_KEY_LEN];
    memset(k, 0x5A, sizeof(k));
    bl_session_init(&c, BL_ROLE_CENTRAL, k, prng, &sc);
    int n = bl_session_start(&c, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, (size_t)n, b, sizeof(b), &bl), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_WAIT_COMMIT);
    CHECK_EQ_INT(bl, 0);

    /* noise */
    uint8_t noise[40];
    memset(noise, 0xC3, sizeof(noise));
    CHECK(bl_pair_rx(&d.r, noise, sizeof(noise), b, sizeof(b), &bl) < 0);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_WAIT_COMMIT);

    /* initiator: a KEY before it has committed */
    n = bl_pair_start(&d.i, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    uint8_t key[BL_MAX_MSG];
    memcpy(key, b, bl);
    size_t keyl = bl;
    /* duplicate COMMIT while waiting for REVEAL */
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, (size_t)n, b, sizeof(b), &bl), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_WAIT_REVEAL);
    /* CONFIRM long before it can mean anything */
    uint8_t tag[32] = {0};
    bl_frame_t cf = {BL_WIRE_VERSION, BL_MSG_PAIR_CONFIRM, 0, 0, 32, tag};
    int cn = bl_frame_encode(&cf, NULL, 0, b, sizeof(b));
    CHECK_EQ_INT(bl_pair_rx(&d.i, b, (size_t)cn, a, sizeof(a), &al), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_WAIT_KEY);
    /* the real KEY still works afterwards */
    CHECK_EQ_INT(bl_pair_rx(&d.i, key, keyl, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&d.r, a, al, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_COMPARE);
    CHECK_EQ_INT(bl_pair_state(&d.r), BL_PS_COMPARE);
    /* accept is only valid in COMPARE, once */
    CHECK_EQ_INT(bl_pair_accept(&d.i, true, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_accept(&d.i, true, a, sizeof(a), &al), BL_E_STATE);
    CHECK_EQ_INT(al, 0);
    /* a duplicated confirm is not double-counted */
    size_t first = 0;
    uint8_t conf[BL_MAX_MSG];
    CHECK_EQ_INT(bl_pair_accept(&d.r, true, conf, sizeof(conf), &first), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&d.i, conf, first, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_DONE);
    CHECK_EQ_INT(bl_pair_rx(&d.i, conf, first, a, sizeof(a), &al), BL_E_STATE);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_DONE);
}

static void t_pair_malformed_rejected(void) {
    uint8_t pl[64], f[BL_MAX_MSG], out[BL_MAX_MSG];
    size_t ol = 0;
    /* COMMIT variants sent to a fresh responder each time */
    struct {
        uint8_t ver;
        uint8_t name_len;
        size_t total;
        uint8_t bad_char;
    } cases[] = {
        {2, 4, 34 + 4, 0}, /* unknown pairing version */
        {1, 5, 34 + 4, 0}, /* name length lies */
        {1, 17, 34 + 17, 0}, /* name too long */
        {1, 4, 34 + 4, 0x0A}, /* newline in the name */
        {1, 4, 20, 0}, /* truncated */
    };
    for(size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        bl_pair_t r;
        prng_t rr = {0xC0u + (uint32_t)k};
        bl_pair_init(&r, BL_PAIR_RESPONDER, "S", prng, &rr, 0);
        memset(pl, 'A', sizeof(pl));
        pl[0] = cases[k].ver;
        pl[33] = cases[k].name_len;
        if(cases[k].bad_char) pl[35] = cases[k].bad_char;
        bl_frame_t fr = {BL_WIRE_VERSION, BL_MSG_PAIR_COMMIT, 0, 0, (uint16_t)cases[k].total, pl};
        int n = bl_frame_encode(&fr, NULL, 0, f, sizeof(f));
        CHECK(bl_pair_rx(&r, f, (size_t)n, out, sizeof(out), &ol) < 0);
        CHECK_EQ_INT(bl_pair_state(&r), BL_PS_WAIT_COMMIT);
        CHECK_EQ_INT(ol, 0);
    }
}

/* A low-order public key from the responder: the initiator stops before
 * revealing anything and tells the responder. */
static void t_pair_low_order_key(void) {
    bl_pair_t i;
    prng_t ri = {0xD1};
    bl_pair_init(&i, BL_PAIR_INITIATOR, "F", prng, &ri, 0);
    uint8_t a[BL_MAX_MSG], out[BL_MAX_MSG];
    size_t ol = 0;
    CHECK(bl_pair_start(&i, a, sizeof(a)) > 0);
    uint8_t pl[50 + 1];
    memset(pl, 0, sizeof(pl));
    pl[0] = BL_PAIR_VERSION; /* pk all zero: a low-order point */
    pl[49] = 1;
    pl[50] = 'S';
    bl_frame_t fr = {BL_WIRE_VERSION, BL_MSG_PAIR_KEY, 0, 0, sizeof(pl), pl};
    int n = bl_frame_encode(&fr, NULL, 0, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&i, a, (size_t)n, out, sizeof(out), &ol), BL_E_CRYPTO);
    CHECK_EQ_INT(i.fail, BL_PF_CRYPTO);
    bl_frame_t sent;
    CHECK_EQ_INT(bl_frame_decode(out, ol, NULL, 0, &sent), BL_OK);
    CHECK_EQ_INT(sent.type, BL_MSG_PAIR_ABORT); /* not a REVEAL */
}

static void t_pair_timeout_and_cancel(void) {
    uint8_t out[BL_MAX_MSG];
    size_t ol = 0;
    bl_pair_t r;
    prng_t rr = {0xE1};
    /* starts just before the tick counter wraps */
    bl_pair_init(&r, BL_PAIR_RESPONDER, "S", prng, &rr, 0xFFFFF000u);
    CHECK(!bl_pair_tick(&r, 0xFFFFF000u + BL_PAIR_TIMEOUT_MS - 1u, out, sizeof(out), &ol));
    CHECK(bl_pair_tick(&r, 0xFFFFF000u + BL_PAIR_TIMEOUT_MS, out, sizeof(out), &ol));
    CHECK_EQ_INT(r.fail, BL_PF_TIMEOUT);
    bl_frame_t f;
    CHECK_EQ_INT(bl_frame_decode(out, ol, NULL, 0, &f), BL_OK);
    CHECK_EQ_INT(f.type, BL_MSG_PAIR_ABORT);
    CHECK_EQ_INT(f.payload[0], BL_PAIR_ABORT_TIMEOUT);
    CHECK(!bl_pair_tick(&r, 0xFFFFF000u + 2u * BL_PAIR_TIMEOUT_MS, out, sizeof(out), &ol));

    /* cancel before anything was sent: no frame */
    bl_pair_t i;
    prng_t ri = {0xE2};
    bl_pair_init(&i, BL_PAIR_INITIATOR, "F", prng, &ri, 0);
    CHECK_EQ_INT(bl_pair_cancel(&i, out, sizeof(out), &ol), BL_OK);
    CHECK_EQ_INT(ol, 0);
    CHECK_EQ_INT(i.fail, BL_PF_CANCELLED);
    /* cancel mid-way: ABORT */
    duo_t d;
    duo_init(&d, 0xE3, 0xE4);
    duo_exchange(&d);
    CHECK_EQ_INT(bl_pair_cancel(&d.r, out, sizeof(out), &ol), BL_OK);
    CHECK(ol > 0);
    size_t xl = 0;
    uint8_t x[BL_MAX_MSG];
    CHECK_EQ_INT(bl_pair_rx(&d.i, out, ol, x, sizeof(x), &xl), BL_OK);
    CHECK_EQ_INT(d.i.peer_reason, BL_PAIR_ABORT_CANCELLED);
    /* a finished attempt cannot be cancelled into a failure */
    duo_t e;
    duo_init(&e, 0xE5, 0xE6);
    duo_exchange(&e);
    duo_answer(&e.i, &e.r, true);
    duo_answer(&e.r, &e.i, true);
    CHECK_EQ_INT(bl_pair_cancel(&e.i, out, sizeof(out), &ol), BL_OK);
    CHECK_EQ_INT(ol, 0);
    CHECK_EQ_INT(bl_pair_state(&e.i), BL_PS_DONE);
}

static void t_pair_local_name_sanitized(void) {
    bl_pair_t p;
    prng_t r = {0xF1};
    bl_pair_init(&p, BL_PAIR_INITIATOR, "Bad\nName\x01 that is far too long", prng, &r, 0);
    CHECK_EQ_INT(p.len_local, BL_PAIR_NAME_MAX);
    CHECK(strcmp(p.name_local, "Bad?Name? that i") == 0);
    bl_pair_init(&p, BL_PAIR_INITIATOR, NULL, prng, &r, 0);
    CHECK_EQ_INT(p.len_local, 0);
    /* an empty name still pairs */
    bl_pair_t q;
    prng_t r2 = {0xF2};
    bl_pair_init(&q, BL_PAIR_RESPONDER, "", prng, &r2, 0);
    uint8_t a[BL_MAX_MSG], b[BL_MAX_MSG];
    size_t al = 0, bl = 0;
    int n = bl_pair_start(&p, a, sizeof(a));
    CHECK_EQ_INT(bl_pair_rx(&q, a, (size_t)n, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&p, b, bl, a, sizeof(a), &al), BL_OK);
    CHECK_EQ_INT(bl_pair_rx(&q, a, al, b, sizeof(b), &bl), BL_OK);
    CHECK_EQ_INT(bl_pair_state(&q), BL_PS_COMPARE);
    CHECK(strcmp(bl_pair_peer_name(&q), "") == 0);
}

static void t_pair_wipe_and_args(void) {
    duo_t d;
    duo_init(&d, 0x0F1, 0x0F2);
    duo_exchange(&d);
    bl_pair_wipe(&d.i);
    CHECK(all_zero((const uint8_t*)&d.i, sizeof(d.i)));
    uint8_t out[8];
    size_t ol = 0;
    CHECK_EQ_INT(bl_pair_init(NULL, BL_PAIR_INITIATOR, "x", prng, NULL, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_pair_init(&d.i, (bl_pair_role_t)7, "x", prng, NULL, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_pair_init(&d.i, BL_PAIR_INITIATOR, "x", NULL, NULL, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_pair_rx(NULL, out, 1, out, sizeof(out), &ol), BL_E_ARG);
    CHECK_EQ_INT(bl_pair_start(&d.r, out, sizeof(out)), BL_E_STATE); /* responder never starts */
    /* output buffer too small for COMMIT: nothing changes */
    prng_t r = {1};
    bl_pair_init(&d.i, BL_PAIR_INITIATOR, "x", prng, &r, 0);
    CHECK(bl_pair_start(&d.i, out, sizeof(out)) < 0);
    CHECK_EQ_INT(bl_pair_state(&d.i), BL_PS_IDLE);
    CHECK_EQ_INT(bl_pair_state(NULL), BL_PS_FAILED);
    CHECK(strcmp(bl_pair_peer_name(NULL), "") == 0);
}

void run_pair_tests(void) {
    RUN(t_pair_happy_path);
    RUN(t_pair_peer_confirms_first);
    RUN(t_pair_rejected);
    RUN(t_pair_man_in_the_middle);
    RUN(t_pair_tampering_changes_the_code);
    RUN(t_pair_commitment_enforced);
    RUN(t_pair_bad_confirm);
    RUN(t_pair_ignores_out_of_order_and_garbage);
    RUN(t_pair_malformed_rejected);
    RUN(t_pair_low_order_key);
    RUN(t_pair_timeout_and_cancel);
    RUN(t_pair_local_name_sanitized);
    RUN(t_pair_wipe_and_args);
}
