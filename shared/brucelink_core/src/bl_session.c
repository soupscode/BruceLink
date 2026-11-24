/*
 * BruceLink shared core — authenticated session state machine.
 * SPDX-License-Identifier: MIT
 */
#include "bl_session.h"

#include "bl_crypto.h"

#include <string.h>

/* Domain-separation labels for the two confirm tags. */
static const char CONFIRM_P[] = "BLcP";
static const char CONFIRM_C[] = "BLcC";
static const char INFO_C2P[] = "brucelink v1 c2p"; /* key central->peripheral */
static const char INFO_P2C[] = "brucelink v1 p2c"; /* key peripheral->central */

/* HELLO payload: proto_min(1) proto_max(1) eph_pub(32) nonce(16) = 50 */
#define HS_HELLO_LEN (2u + 32u + BL_NONCE_LEN)
/* HELLO_ACK payload: proto_sel(1) eph_pub(32) nonce(16) confirm(32) = 81 */
#define HS_ACK_LEN (1u + 32u + BL_NONCE_LEN + BL_SHA256_LEN)
/* AUTH payload: confirm(32) */
#define HS_AUTH_LEN (BL_SHA256_LEN)

/* hello_min | hello_max | proto_sel | pub_c | nonce_c | pub_p | nonce_p.
 * Every HELLO / HELLO_ACK field is in it (review 2026-11-23: the offered
 * range was not, so a rewritten HELLO could downgrade the version). */
#define TRANSCRIPT_LEN (3u + 32u + BL_NONCE_LEN + 32u + BL_NONCE_LEN)

static void build_transcript(
    const bl_session_t* s,
    uint8_t proto_sel,
    const uint8_t pub_c[32],
    const uint8_t nonce_c[BL_NONCE_LEN],
    const uint8_t pub_p[32],
    const uint8_t nonce_p[BL_NONCE_LEN],
    uint8_t out[TRANSCRIPT_LEN]) {
    size_t o = 0;
    out[o++] = s->hello_min;
    out[o++] = s->hello_max;
    out[o++] = proto_sel;
    memcpy(out + o, pub_c, 32);
    o += 32;
    memcpy(out + o, nonce_c, BL_NONCE_LEN);
    o += BL_NONCE_LEN;
    memcpy(out + o, pub_p, 32);
    o += 32;
    memcpy(out + o, nonce_p, BL_NONCE_LEN);
}

static int derive_keys(bl_session_t* s, uint8_t proto_sel) {
    /* ECDH */
    uint8_t shared[32];
    const uint8_t* mine = s->eph_sk;
    int rc = bl_x25519(shared, mine, s->peer_pk);
    if(rc != BL_OK) return rc;

    /* ikm = shared || psk */
    uint8_t ikm[32 + BL_KEY_LEN];
    memcpy(ikm, shared, 32);
    memcpy(ikm + 32, s->psk, BL_KEY_LEN);

    /* salt = nonce_c || nonce_p (ordered by role, not by who is local) */
    uint8_t salt[2 * BL_NONCE_LEN];
    const uint8_t* nonce_c = (s->role == BL_ROLE_CENTRAL) ? s->nonce_local : s->nonce_peer;
    const uint8_t* nonce_p = (s->role == BL_ROLE_CENTRAL) ? s->nonce_peer : s->nonce_local;
    memcpy(salt, nonce_c, BL_NONCE_LEN);
    memcpy(salt + BL_NONCE_LEN, nonce_p, BL_NONCE_LEN);

    uint8_t prk[BL_SHA256_LEN];
    bl_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);

    uint8_t k_c2p[BL_KEY_LEN];
    uint8_t k_p2c[BL_KEY_LEN];
    uint8_t info[sizeof(INFO_C2P) - 1 + 1];
    memcpy(info, INFO_C2P, sizeof(INFO_C2P) - 1);
    info[sizeof(INFO_C2P) - 1] = proto_sel;
    bl_hkdf_expand(prk, info, sizeof(info), k_c2p, BL_KEY_LEN);
    memcpy(info, INFO_P2C, sizeof(INFO_P2C) - 1);
    info[sizeof(INFO_P2C) - 1] = proto_sel;
    bl_hkdf_expand(prk, info, sizeof(info), k_p2c, BL_KEY_LEN);

    if(s->role == BL_ROLE_CENTRAL) {
        memcpy(s->tx_key, k_c2p, BL_KEY_LEN);
        memcpy(s->rx_key, k_p2c, BL_KEY_LEN);
    } else {
        memcpy(s->tx_key, k_p2c, BL_KEY_LEN);
        memcpy(s->rx_key, k_c2p, BL_KEY_LEN);
    }
    s->tx_counter = 1;
    s->rx_highest = 0;
    s->proto_version = proto_sel;
    s->keys_ready = true;

    bl_secure_zero(shared, sizeof(shared));
    bl_secure_zero(ikm, sizeof(ikm));
    bl_secure_zero(prk, sizeof(prk));
    bl_secure_zero(k_c2p, sizeof(k_c2p));
    bl_secure_zero(k_p2c, sizeof(k_p2c));
    return BL_OK;
}

static void confirm_tag(
    const bl_session_t* s,
    const char label[4],
    const uint8_t* transcript,
    size_t tlen,
    uint8_t out[BL_SHA256_LEN]) {
    bl_hmac_ctx c;
    bl_hmac_init(&c, s->psk, BL_KEY_LEN);
    bl_hmac_update(&c, label, 4);
    bl_hmac_update(&c, transcript, tlen);
    bl_hmac_final(&c, out);
}

/* A failed session can do nothing more: wipe its secrets now rather than
 * when the caller gets round to closing it. */
static int session_fail(bl_session_t* s, int rc) {
    bl_secure_zero(s->psk, sizeof(s->psk));
    bl_secure_zero(s->eph_sk, sizeof(s->eph_sk));
    bl_secure_zero(s->tx_key, sizeof(s->tx_key));
    bl_secure_zero(s->rx_key, sizeof(s->rx_key));
    s->keys_ready = false;
    s->state = BL_SS_FAILED;
    return rc;
}

/* Verify and accept a sealed frame: keys must exist, the caller decides
 * which states may do this. */
static int session_open_frame(bl_session_t* s, const uint8_t* in, size_t in_len, bl_frame_t* f) {
    if(!s->keys_ready) return BL_E_STATE;
    int rc = bl_frame_decode(in, in_len, s->rx_key, BL_KEY_LEN, f);
    if(rc != BL_OK) return rc;
    if(f->version != BL_WIRE_VERSION) return BL_E_VERSION;
    /* strictly increasing counter: rejects replays and reorders */
    if(f->seq <= s->rx_highest) return BL_E_REPLAY;
    s->rx_highest = f->seq;
    return BL_OK;
}

int bl_session_init(
    bl_session_t* s,
    bl_role_t role,
    const uint8_t psk[BL_KEY_LEN],
    bl_rng_fn rng,
    void* rng_ctx) {
    if(s == NULL || psk == NULL || rng == NULL) return BL_E_ARG;
    memset(s, 0, sizeof(*s));
    s->role = role;
    memcpy(s->psk, psk, BL_KEY_LEN);
    rng(rng_ctx, s->eph_sk, sizeof(s->eph_sk));
    /* X25519 clamps internally; base-point mult for our public key. */
    int rc = bl_x25519_base(s->eph_pk, s->eph_sk);
    if(rc != BL_OK) return rc;
    rng(rng_ctx, s->nonce_local, sizeof(s->nonce_local));
    s->state = (role == BL_ROLE_CENTRAL) ? BL_SS_INIT : BL_SS_WAIT_HELLO;
    return BL_OK;
}

int bl_session_start(bl_session_t* s, uint8_t* out, size_t out_cap) {
    if(s == NULL) return BL_E_ARG;
    if(s->role != BL_ROLE_CENTRAL || s->state != BL_SS_INIT) return BL_E_STATE;
    uint8_t payload[HS_HELLO_LEN];
    s->hello_min = BL_PROTO_MIN;
    s->hello_max = BL_PROTO_MAX;
    payload[0] = s->hello_min;
    payload[1] = s->hello_max;
    memcpy(payload + 2, s->eph_pk, 32);
    memcpy(payload + 2 + 32, s->nonce_local, BL_NONCE_LEN);
    bl_frame_t f = {
        .version = BL_WIRE_VERSION,
        .type = BL_MSG_HELLO,
        .flags = BL_FLAG_NONE,
        .seq = 0,
        .payload_len = HS_HELLO_LEN,
        .payload = payload};
    int n = bl_frame_encode(&f, NULL, 0, out, out_cap);
    if(n < 0) return n;
    s->state = BL_SS_WAIT_HELLO_ACK;
    return n;
}

static uint8_t pick_proto(uint8_t peer_min, uint8_t peer_max) {
    uint8_t lo = peer_min > BL_PROTO_MIN ? peer_min : BL_PROTO_MIN;
    uint8_t hi = peer_max < BL_PROTO_MAX ? peer_max : BL_PROTO_MAX;
    if(lo > hi) return 0; /* no overlap */
    return hi; /* highest common */
}

int bl_session_handshake_rx(
    bl_session_t* s,
    const uint8_t* in,
    size_t in_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(s == NULL || out_len == NULL) return BL_E_ARG;
    *out_len = 0;
    bl_frame_t f;
    int rc = bl_frame_decode(in, in_len, NULL, 0, &f);
    if(rc != BL_OK) return rc;
    if(f.version != BL_WIRE_VERSION) return BL_E_VERSION;

    uint8_t transcript[TRANSCRIPT_LEN];

    switch(s->state) {
    case BL_SS_WAIT_HELLO: { /* peripheral */
        if(f.type != BL_MSG_HELLO || f.payload_len != HS_HELLO_LEN) return BL_E_STATE;
        s->hello_min = f.payload[0];
        s->hello_max = f.payload[1];
        uint8_t proto = pick_proto(s->hello_min, s->hello_max);
        if(proto == 0) return session_fail(s, BL_E_VERSION);
        memcpy(s->peer_pk, f.payload + 2, 32);
        memcpy(s->nonce_peer, f.payload + 2 + 32, BL_NONCE_LEN);
        rc = derive_keys(s, proto);
        if(rc != BL_OK) return session_fail(s, rc);
        /* transcript uses central's pub/nonce then peripheral's */
        build_transcript(
            s, proto, s->peer_pk, s->nonce_peer, s->eph_pk, s->nonce_local, transcript);
        uint8_t confirm_p[BL_SHA256_LEN];
        confirm_tag(s, CONFIRM_P, transcript, sizeof(transcript), confirm_p);
        uint8_t payload[HS_ACK_LEN];
        payload[0] = proto;
        memcpy(payload + 1, s->eph_pk, 32);
        memcpy(payload + 1 + 32, s->nonce_local, BL_NONCE_LEN);
        memcpy(payload + 1 + 32 + BL_NONCE_LEN, confirm_p, BL_SHA256_LEN);
        bl_frame_t r = {
            .version = BL_WIRE_VERSION,
            .type = BL_MSG_HELLO_ACK,
            .flags = BL_FLAG_RESPONSE,
            .seq = 0,
            .payload_len = HS_ACK_LEN,
            .payload = payload};
        int n = bl_frame_encode(&r, NULL, 0, out, out_cap);
        bl_secure_zero(confirm_p, sizeof(confirm_p));
        if(n < 0) return n;
        *out_len = (size_t)n;
        s->state = BL_SS_WAIT_AUTH;
        return BL_OK;
    }
    case BL_SS_WAIT_HELLO_ACK: { /* central */
        if(f.type != BL_MSG_HELLO_ACK || f.payload_len != HS_ACK_LEN) return BL_E_STATE;
        uint8_t proto = f.payload[0];
        if(proto < BL_PROTO_MIN || proto > BL_PROTO_MAX) return session_fail(s, BL_E_VERSION);
        memcpy(s->peer_pk, f.payload + 1, 32);
        memcpy(s->nonce_peer, f.payload + 1 + 32, BL_NONCE_LEN);
        const uint8_t* confirm_p = f.payload + 1 + 32 + BL_NONCE_LEN;
        rc = derive_keys(s, proto);
        if(rc != BL_OK) return session_fail(s, rc);
        build_transcript(
            s, proto, s->eph_pk, s->nonce_local, s->peer_pk, s->nonce_peer, transcript);
        uint8_t expect_p[BL_SHA256_LEN];
        confirm_tag(s, CONFIRM_P, transcript, sizeof(transcript), expect_p);
        if(!bl_ct_equal(expect_p, confirm_p, BL_SHA256_LEN)) {
            bl_secure_zero(expect_p, sizeof(expect_p));
            s->auth_failures++;
            return session_fail(s, BL_E_AUTH);
        }
        bl_secure_zero(expect_p, sizeof(expect_p));
        /* send AUTH with confirm_C */
        uint8_t confirm_c[BL_SHA256_LEN];
        confirm_tag(s, CONFIRM_C, transcript, sizeof(transcript), confirm_c);
        bl_frame_t r = {
            .version = BL_WIRE_VERSION,
            .type = BL_MSG_AUTH,
            .flags = BL_FLAG_NONE,
            .seq = 0,
            .payload_len = HS_AUTH_LEN,
            .payload = confirm_c};
        int n = bl_frame_encode(&r, NULL, 0, out, out_cap);
        bl_secure_zero(confirm_c, sizeof(confirm_c));
        if(n < 0) return n;
        *out_len = (size_t)n;
        s->state = BL_SS_WAIT_AUTH_OK;
        return BL_OK;
    }
    case BL_SS_WAIT_AUTH: { /* peripheral */
        if(f.type != BL_MSG_AUTH || f.payload_len != HS_AUTH_LEN) return BL_E_STATE;
        build_transcript(
            s,
            s->proto_version,
            s->peer_pk,
            s->nonce_peer,
            s->eph_pk,
            s->nonce_local,
            transcript);
        uint8_t expect_c[BL_SHA256_LEN];
        confirm_tag(s, CONFIRM_C, transcript, sizeof(transcript), expect_c);
        if(!bl_ct_equal(expect_c, f.payload, BL_SHA256_LEN)) {
            bl_secure_zero(expect_c, sizeof(expect_c));
            s->auth_failures++;
            return session_fail(s, BL_E_AUTH);
        }
        bl_secure_zero(expect_c, sizeof(expect_c));
        /* emit authenticated AUTH_OK as first sealed frame */
        s->state = BL_SS_ESTABLISHED;
        int n = bl_session_seal(s, BL_MSG_AUTH_OK, BL_FLAG_RESPONSE, NULL, 0, out, out_cap);
        if(n < 0) return session_fail(s, n);
        *out_len = (size_t)n;
        return BL_OK;
    }
    case BL_SS_WAIT_AUTH_OK: { /* central: AUTH_OK is a sealed data frame */
        /* Only an AUTH_OK can end the handshake, well or badly. Anything
         * else (a repeated HELLO_ACK, noise) is ignored like in the other
         * states (review 2026-11-23: it used to be terminal). */
        if(f.type != BL_MSG_AUTH_OK) return BL_E_STATE;
        bl_frame_t df;
        rc = session_open_frame(s, in, in_len, &df);
        if(rc != BL_OK) {
            if(rc == BL_E_MAC) s->auth_failures++;
            return session_fail(s, rc);
        }
        s->state = BL_SS_ESTABLISHED;
        return BL_OK;
    }
    default: return BL_E_STATE;
    }
}

int bl_session_seal(
    bl_session_t* s,
    uint8_t type,
    uint8_t flags,
    const uint8_t* payload,
    uint16_t payload_len,
    uint8_t* out,
    size_t out_cap) {
    return bl_session_seal_tag(s, type, flags, 0, payload, payload_len, out, out_cap);
}

int bl_session_seal_tag(
    bl_session_t* s,
    uint8_t type,
    uint8_t flags,
    uint8_t tag,
    const uint8_t* payload,
    uint16_t payload_len,
    uint8_t* out,
    size_t out_cap) {
    if(s == NULL) return BL_E_ARG;
    /* Only an established session sends (the responder's AUTH_OK is sealed
     * right after it becomes established). */
    if(!s->keys_ready || s->state != BL_SS_ESTABLISHED) return BL_E_STATE;
    if(s->tx_counter == 0xFFFFFFFFu) return BL_E_STATE; /* counter exhausted */
    bl_frame_t f = {
        .version = BL_WIRE_VERSION,
        .type = type,
        .flags = flags,
        .seq = s->tx_counter,
        .payload_len = payload_len,
        .payload = payload,
        .tag = tag};
    int n = bl_frame_encode(&f, s->tx_key, BL_KEY_LEN, out, out_cap);
    if(n < 0) return n;
    s->tx_counter++;
    return n;
}

int bl_session_open(bl_session_t* s, const uint8_t* in, size_t in_len, bl_frame_t* f) {
    if(s == NULL || f == NULL) return BL_E_ARG;
    /* Not before the peer has proved the key (review 2026-11-23: keys
     * exist from HELLO on, so a half-done session used to open frames). */
    if(s->state != BL_SS_ESTABLISHED) return BL_E_STATE;
    return session_open_frame(s, in, in_len, f);
}

void bl_session_close(bl_session_t* s) {
    if(s == NULL) return;
    bl_secure_zero(s->psk, sizeof(s->psk));
    bl_secure_zero(s->eph_sk, sizeof(s->eph_sk));
    bl_secure_zero(s->tx_key, sizeof(s->tx_key));
    bl_secure_zero(s->rx_key, sizeof(s->rx_key));
    s->keys_ready = false;
    s->state = BL_SS_CLOSED;
}
