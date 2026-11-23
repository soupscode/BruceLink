/*
 * BruceLink shared core — pairing (see bl_pair.h and ADR 0005).
 * SPDX-License-Identifier: MIT
 */
#include "bl_pair.h"

#include "bl_crypto.h"

#include <string.h>

static const char L_COMMIT[] = "BLpair1 commit";
static const char L_TRANSCRIPT[] = "BLpair1 transcript";
static const char L_CODE[] = "BLpair1 code";
static const char L_PSK[] = "BLpair1 psk";
static const char L_CONFIRM_I[] = "BLpair1 confirm I";
static const char L_CONFIRM_R[] = "BLpair1 confirm R";
static const char L_KEY_ID[] = "BLpair1 key id";

/* Payload sizes (without the variable-length name). */
#define COMMIT_FIXED (1u + 32u + 1u) /* ver, commit, name_len */
#define KEY_FIXED (1u + 32u + BL_NONCE_LEN + 1u) /* ver, pk, nonce, name_len */
#define REVEAL_LEN (32u + BL_NONCE_LEN)
#define CONFIRM_LEN (BL_SHA256_LEN)
#define ABORT_LEN 1u

static bool terminal(const bl_pair_t* p) {
    return p->state == BL_PS_DONE || p->state == BL_PS_FAILED;
}

static void fail(bl_pair_t* p, bl_pair_fail_t why) {
    p->state = BL_PS_FAILED;
    p->fail = why;
    /* Nothing from a failed attempt may be used: drop all secrets now. */
    bl_secure_zero(p->sk, sizeof(p->sk));
    bl_secure_zero(p->psk, sizeof(p->psk));
}

static int encode(uint8_t type, const uint8_t* pl, uint16_t len, uint8_t* out, size_t cap) {
    bl_frame_t f = {
        .version = BL_WIRE_VERSION,
        .type = type,
        .flags = BL_FLAG_NONE,
        .seq = 0,
        .payload_len = len,
        .payload = pl};
    return bl_frame_encode(&f, NULL, 0, out, cap);
}

static int emit(
    uint8_t type,
    const uint8_t* pl,
    uint16_t len,
    uint8_t* out,
    size_t cap,
    size_t* out_len) {
    int n = encode(type, pl, len, out, cap);
    if(n < 0) return n;
    *out_len = (size_t)n;
    return BL_OK;
}

static int emit_abort(bl_pair_t* p, uint8_t reason, uint8_t* out, size_t cap, size_t* out_len) {
    (void)p;
    return emit(BL_MSG_PAIR_ABORT, &reason, ABORT_LEN, out, cap, out_len);
}

static bool printable(uint8_t c) {
    return c >= 0x20u && c <= 0x7Eu;
}

static void commitment(const uint8_t pk[32], const uint8_t n[BL_NONCE_LEN], uint8_t out[32]) {
    const uint8_t ver = BL_PAIR_VERSION;
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    bl_sha256_update(&c, L_COMMIT, sizeof(L_COMMIT) - 1);
    bl_sha256_update(&c, &ver, 1);
    bl_sha256_update(&c, pk, 32);
    bl_sha256_update(&c, n, BL_NONCE_LEN);
    bl_sha256_final(&c, out);
}

/* Everything both sides exchanged, in initiator-then-responder order. */
static void build_transcript(bl_pair_t* p) {
    const bool ini = p->role == BL_PAIR_INITIATOR;
    const uint8_t ver = BL_PAIR_VERSION;
    const uint8_t len_i = ini ? p->len_local : p->len_peer;
    const uint8_t len_r = ini ? p->len_peer : p->len_local;
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    bl_sha256_update(&c, L_TRANSCRIPT, sizeof(L_TRANSCRIPT) - 1);
    bl_sha256_update(&c, &ver, 1);
    bl_sha256_update(&c, &len_i, 1);
    bl_sha256_update(&c, ini ? p->name_local : p->name_peer, len_i);
    bl_sha256_update(&c, ini ? p->pk : p->pk_peer, 32);
    bl_sha256_update(&c, ini ? p->n_local : p->n_peer, BL_NONCE_LEN);
    bl_sha256_update(&c, &len_r, 1);
    bl_sha256_update(&c, ini ? p->name_peer : p->name_local, len_r);
    bl_sha256_update(&c, ini ? p->pk_peer : p->pk, 32);
    bl_sha256_update(&c, ini ? p->n_peer : p->n_local, BL_NONCE_LEN);
    bl_sha256_final(&c, p->transcript);
}

static void confirm_tag(const bl_pair_t* p, bl_pair_role_t who, uint8_t out[BL_SHA256_LEN]) {
    const char* label = who == BL_PAIR_INITIATOR ? L_CONFIRM_I : L_CONFIRM_R;
    bl_hmac_ctx c;
    bl_hmac_init(&c, p->psk, BL_KEY_LEN);
    bl_hmac_update(&c, label, sizeof(L_CONFIRM_I) - 1); /* both labels are the same length */
    bl_hmac_update(&c, p->transcript, sizeof(p->transcript));
    bl_hmac_final(&c, out);
}

/* Both public keys and nonces known: derive the code and the key. */
static int derive(bl_pair_t* p) {
    uint8_t shared[32];
    int rc = bl_x25519(shared, p->sk, p->pk_peer);
    bl_secure_zero(p->sk, sizeof(p->sk)); /* used exactly once */
    if(rc != BL_OK) {
        bl_secure_zero(shared, sizeof(shared));
        return rc;
    }
    build_transcript(p);

    uint8_t h[BL_SHA256_LEN];
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    bl_sha256_update(&c, L_CODE, sizeof(L_CODE) - 1);
    bl_sha256_update(&c, p->transcript, sizeof(p->transcript));
    bl_sha256_final(&c, h);
    const uint32_t v = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) |
                       ((uint32_t)h[3] << 24);
    p->code = v % BL_PAIR_CODE_MOD;

    const bool ini = p->role == BL_PAIR_INITIATOR;
    uint8_t salt[2 * BL_NONCE_LEN];
    memcpy(salt, ini ? p->n_local : p->n_peer, BL_NONCE_LEN);
    memcpy(salt + BL_NONCE_LEN, ini ? p->n_peer : p->n_local, BL_NONCE_LEN);
    uint8_t prk[BL_SHA256_LEN];
    bl_hkdf_extract(salt, sizeof(salt), shared, sizeof(shared), prk);
    uint8_t info[sizeof(L_PSK) - 1 + 32];
    memcpy(info, L_PSK, sizeof(L_PSK) - 1);
    memcpy(info + sizeof(L_PSK) - 1, p->transcript, 32);
    rc = bl_hkdf_expand(prk, info, sizeof(info), p->psk, BL_KEY_LEN);

    bl_secure_zero(shared, sizeof(shared));
    bl_secure_zero(prk, sizeof(prk));
    bl_secure_zero(h, sizeof(h));
    return rc;
}

static void set_name(char* dst, uint8_t* dst_len, const char* src) {
    uint8_t n = 0;
    if(src != NULL) {
        while(n < BL_PAIR_NAME_MAX && src[n] != '\0') {
            const uint8_t c = (uint8_t)src[n];
            dst[n] = printable(c) ? (char)c : '?';
            n++;
        }
    }
    dst[n] = '\0';
    *dst_len = n;
}

/* Copy a received name; refuse anything that is not printable ASCII. */
static bool take_peer_name(bl_pair_t* p, const uint8_t* src, uint8_t len) {
    if(len > BL_PAIR_NAME_MAX) return false;
    for(uint8_t i = 0; i < len; i++) {
        if(!printable(src[i])) return false;
    }
    memcpy(p->name_peer, src, len);
    p->name_peer[len] = '\0';
    p->len_peer = len;
    return true;
}

int bl_pair_init(
    bl_pair_t* p,
    bl_pair_role_t role,
    const char* local_name,
    bl_rng_fn rng,
    void* rng_ctx,
    uint32_t now_ms) {
    if(p == NULL || rng == NULL) return BL_E_ARG;
    if(role != BL_PAIR_INITIATOR && role != BL_PAIR_RESPONDER) return BL_E_ARG;
    memset(p, 0, sizeof(*p));
    p->role = role;
    p->started_ms = now_ms;
    set_name(p->name_local, &p->len_local, local_name);
    rng(rng_ctx, p->sk, sizeof(p->sk));
    int rc = bl_x25519_base(p->pk, p->sk);
    if(rc != BL_OK) {
        bl_pair_wipe(p);
        p->state = BL_PS_FAILED;
        p->fail = BL_PF_CRYPTO;
        return rc;
    }
    rng(rng_ctx, p->n_local, sizeof(p->n_local));
    p->state = role == BL_PAIR_INITIATOR ? BL_PS_IDLE : BL_PS_WAIT_COMMIT;
    return BL_OK;
}

int bl_pair_start(bl_pair_t* p, uint8_t* out, size_t out_cap) {
    if(p == NULL || out == NULL) return BL_E_ARG;
    if(p->role != BL_PAIR_INITIATOR || p->state != BL_PS_IDLE) return BL_E_STATE;
    uint8_t pl[COMMIT_FIXED + BL_PAIR_NAME_MAX];
    pl[0] = BL_PAIR_VERSION;
    commitment(p->pk, p->n_local, pl + 1);
    pl[33] = p->len_local;
    memcpy(pl + COMMIT_FIXED, p->name_local, p->len_local);
    int n = encode(BL_MSG_PAIR_COMMIT, pl, (uint16_t)(COMMIT_FIXED + p->len_local), out, out_cap);
    if(n < 0) return n;
    p->state = BL_PS_WAIT_KEY;
    return n;
}

/* PAIR_CONFIRM from the peer, in COMPARE or WAIT_PEER. */
static int rx_confirm(bl_pair_t* p, const bl_frame_t* f) {
    if(f->payload_len != CONFIRM_LEN) return BL_E_FORMAT;
    if(p->peer_confirmed) return BL_E_STATE; /* duplicate */
    uint8_t expect[BL_SHA256_LEN];
    confirm_tag(p, p->role == BL_PAIR_INITIATOR ? BL_PAIR_RESPONDER : BL_PAIR_INITIATOR, expect);
    const bool ok = bl_ct_equal(expect, f->payload, BL_SHA256_LEN);
    bl_secure_zero(expect, sizeof(expect));
    if(!ok) {
        /* The peer holds a different key: someone is in the middle, or a
         * bug. Never keep this key, even if the local user accepted. */
        fail(p, BL_PF_MISMATCH);
        return BL_E_AUTH;
    }
    p->peer_confirmed = true;
    if(p->state == BL_PS_WAIT_PEER) p->state = BL_PS_DONE;
    return BL_OK;
}

int bl_pair_rx(
    bl_pair_t* p,
    const uint8_t* in,
    size_t in_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len) {
    if(p == NULL || out_len == NULL) return BL_E_ARG;
    *out_len = 0;
    if(terminal(p)) return BL_E_STATE;
    bl_frame_t f;
    int rc = bl_frame_decode(in, in_len, NULL, 0, &f);
    if(rc != BL_OK) return rc;
    if(f.version != BL_WIRE_VERSION) return BL_E_VERSION;

    if(f.type == BL_MSG_PAIR_ABORT) {
        if(f.payload_len != ABORT_LEN) return BL_E_FORMAT;
        p->peer_reason = f.payload[0];
        fail(p, BL_PF_PEER_REJECTED);
        return BL_OK;
    }

    switch(p->state) {
    case BL_PS_WAIT_COMMIT: { /* responder */
        if(f.type != BL_MSG_PAIR_COMMIT) return BL_E_STATE;
        if(f.payload_len < COMMIT_FIXED) return BL_E_SHORT;
        if(f.payload[0] != BL_PAIR_VERSION) return BL_E_VERSION;
        const uint8_t nlen = f.payload[33];
        if(f.payload_len != COMMIT_FIXED + nlen) return BL_E_FORMAT;
        if(!take_peer_name(p, f.payload + COMMIT_FIXED, nlen)) return BL_E_FORMAT;
        memcpy(p->commit_peer, f.payload + 1, 32);

        uint8_t pl[KEY_FIXED + BL_PAIR_NAME_MAX];
        pl[0] = BL_PAIR_VERSION;
        memcpy(pl + 1, p->pk, 32);
        memcpy(pl + 33, p->n_local, BL_NONCE_LEN);
        pl[33 + BL_NONCE_LEN] = p->len_local;
        memcpy(pl + KEY_FIXED, p->name_local, p->len_local);
        rc = emit(
            BL_MSG_PAIR_KEY, pl, (uint16_t)(KEY_FIXED + p->len_local), out, out_cap, out_len);
        if(rc != BL_OK) return rc;
        p->state = BL_PS_WAIT_REVEAL;
        return BL_OK;
    }
    case BL_PS_WAIT_KEY: { /* initiator */
        if(f.type != BL_MSG_PAIR_KEY) return BL_E_STATE;
        if(f.payload_len < KEY_FIXED) return BL_E_SHORT;
        if(f.payload[0] != BL_PAIR_VERSION) return BL_E_VERSION;
        const uint8_t nlen = f.payload[33 + BL_NONCE_LEN];
        if(f.payload_len != KEY_FIXED + nlen) return BL_E_FORMAT;
        if(!take_peer_name(p, f.payload + KEY_FIXED, nlen)) return BL_E_FORMAT;
        memcpy(p->pk_peer, f.payload + 1, 32);
        memcpy(p->n_peer, f.payload + 33, BL_NONCE_LEN);
        if(derive(p) != BL_OK) {
            /* Unusable responder key: tell it, reveal nothing. */
            fail(p, BL_PF_CRYPTO);
            (void)emit_abort(p, BL_PAIR_ABORT_MISMATCH, out, out_cap, out_len);
            return BL_E_CRYPTO;
        }
        uint8_t pl[REVEAL_LEN];
        memcpy(pl, p->pk, 32);
        memcpy(pl + 32, p->n_local, BL_NONCE_LEN);
        rc = emit(BL_MSG_PAIR_REVEAL, pl, REVEAL_LEN, out, out_cap, out_len);
        if(rc != BL_OK) {
            /* The secret half is already spent; this attempt cannot go on. */
            fail(p, BL_PF_CANCELLED);
            return rc;
        }
        p->state = BL_PS_COMPARE;
        return BL_OK;
    }
    case BL_PS_WAIT_REVEAL: { /* responder */
        if(f.type != BL_MSG_PAIR_REVEAL) return BL_E_STATE;
        if(f.payload_len != REVEAL_LEN) return BL_E_FORMAT;
        uint8_t expect[32];
        commitment(f.payload, f.payload + 32, expect);
        const bool ok = bl_ct_equal(expect, p->commit_peer, 32);
        if(!ok) {
            /* The initiator's key is not the one it committed to. */
            fail(p, BL_PF_MISMATCH);
            (void)emit_abort(p, BL_PAIR_ABORT_MISMATCH, out, out_cap, out_len);
            return BL_E_AUTH;
        }
        memcpy(p->pk_peer, f.payload, 32);
        memcpy(p->n_peer, f.payload + 32, BL_NONCE_LEN);
        if(derive(p) != BL_OK) {
            fail(p, BL_PF_CRYPTO);
            (void)emit_abort(p, BL_PAIR_ABORT_MISMATCH, out, out_cap, out_len);
            return BL_E_CRYPTO;
        }
        p->state = BL_PS_COMPARE;
        return BL_OK;
    }
    case BL_PS_COMPARE:
    case BL_PS_WAIT_PEER:
        if(f.type != BL_MSG_PAIR_CONFIRM) return BL_E_STATE;
        return rx_confirm(p, &f);
    default:
        return BL_E_STATE;
    }
}

bool bl_pair_code(const bl_pair_t* p, uint32_t* code) {
    if(p == NULL || code == NULL) return false;
    if(p->state != BL_PS_COMPARE && p->state != BL_PS_WAIT_PEER && p->state != BL_PS_DONE) {
        return false;
    }
    *code = p->code;
    return true;
}

int bl_pair_accept(bl_pair_t* p, bool accept, uint8_t* out, size_t out_cap, size_t* out_len) {
    if(p == NULL || out_len == NULL) return BL_E_ARG;
    *out_len = 0;
    if(p->state != BL_PS_COMPARE) return BL_E_STATE;
    if(!accept) {
        int rc = emit_abort(p, BL_PAIR_ABORT_REJECTED, out, out_cap, out_len);
        fail(p, BL_PF_REJECTED);
        return rc;
    }
    uint8_t tag[BL_SHA256_LEN];
    confirm_tag(p, p->role, tag);
    int rc = emit(BL_MSG_PAIR_CONFIRM, tag, CONFIRM_LEN, out, out_cap, out_len);
    bl_secure_zero(tag, sizeof(tag));
    if(rc != BL_OK) return rc;
    p->state = p->peer_confirmed ? BL_PS_DONE : BL_PS_WAIT_PEER;
    return BL_OK;
}

int bl_pair_cancel(bl_pair_t* p, uint8_t* out, size_t out_cap, size_t* out_len) {
    if(p == NULL || out_len == NULL) return BL_E_ARG;
    *out_len = 0;
    if(terminal(p)) return BL_OK;
    /* IDLE: nothing was sent, so there is nobody to tell. */
    int rc = p->state == BL_PS_IDLE ? BL_OK :
                                      emit_abort(p, BL_PAIR_ABORT_CANCELLED, out, out_cap, out_len);
    fail(p, BL_PF_CANCELLED);
    return rc;
}

bool bl_pair_tick(bl_pair_t* p, uint32_t now_ms, uint8_t* out, size_t out_cap, size_t* out_len) {
    if(p == NULL || out_len == NULL) return false;
    *out_len = 0;
    if(terminal(p)) return false;
    if((uint32_t)(now_ms - p->started_ms) < BL_PAIR_TIMEOUT_MS) return false;
    if(p->state != BL_PS_IDLE) (void)emit_abort(p, BL_PAIR_ABORT_TIMEOUT, out, out_cap, out_len);
    fail(p, BL_PF_TIMEOUT);
    return true;
}

const char* bl_pair_peer_name(const bl_pair_t* p) {
    return p ? p->name_peer : "";
}

void bl_pair_key_id(const uint8_t psk[BL_KEY_LEN], uint8_t key_id[BL_PAIR_KEY_ID_LEN]) {
    uint8_t h[BL_SHA256_LEN];
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    bl_sha256_update(&c, L_KEY_ID, sizeof(L_KEY_ID) - 1);
    bl_sha256_update(&c, psk, BL_KEY_LEN);
    bl_sha256_final(&c, h);
    memcpy(key_id, h, BL_PAIR_KEY_ID_LEN);
    bl_secure_zero(h, sizeof(h));
}

int bl_pair_result(
    const bl_pair_t* p,
    uint8_t psk[BL_KEY_LEN],
    uint8_t key_id[BL_PAIR_KEY_ID_LEN]) {
    if(p == NULL || psk == NULL || key_id == NULL) return BL_E_ARG;
    if(p->state != BL_PS_DONE) return BL_E_STATE;
    memcpy(psk, p->psk, BL_KEY_LEN);
    bl_pair_key_id(p->psk, key_id);
    return BL_OK;
}

void bl_pair_wipe(bl_pair_t* p) {
    if(p != NULL) bl_secure_zero(p, sizeof(*p));
}
