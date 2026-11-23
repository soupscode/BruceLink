/*
 * BruceLink shared core — authenticated session state machine.
 *
 * Mutual authentication with a provisioned 32-byte pre-shared pairing
 * key (PSK), forward secrecy via ephemeral X25519, and per-direction
 * replay protection via monotonic frame counters.
 *
 * Handshake. C = BL_ROLE_CENTRAL, the session initiator (the Flipper);
 * P = BL_ROLE_PERIPHERAL, the responder (the StickS3). These are session
 * roles and are the reverse of the BLE roles (ADR 0001: the Flipper is the
 * BLE peripheral, the StickS3 the BLE central).
 *   C->P HELLO     : proto_min, proto_max, eph_pub_C(32), nonce_C(16)
 *   P->C HELLO_ACK : proto_sel,            eph_pub_P(32), nonce_P(16),
 *                    confirm_P(32) = HMAC(PSK, "BLcP" || transcript)
 *   C->P AUTH      : confirm_C(32) = HMAC(PSK, "BLcC" || transcript)
 *   P->C AUTH_OK   : (authenticated frame; establishes the session)
 *
 * transcript = proto_sel || eph_pub_C || nonce_C || eph_pub_P || nonce_P
 * master = HKDF-Extract(salt = nonce_C||nonce_P,
 *                       ikm   = ECDH(eph_C,eph_P) || PSK)
 * tx/rx keys = HKDF-Expand(master, "brucelink v1 <dir>", 32) each.
 *
 * The caller owns all transport I/O; this module only produces and
 * consumes frame byte buffers.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BL_ROLE_CENTRAL = 0, /* session initiator: the Flipper (sends HELLO) */
    BL_ROLE_PERIPHERAL = 1, /* session responder: the StickS3 */
} bl_role_t;

typedef enum {
    BL_SS_INIT = 0,
    BL_SS_WAIT_HELLO, /* peripheral: waiting for HELLO */
    BL_SS_WAIT_HELLO_ACK, /* central: sent HELLO, waiting ACK */
    BL_SS_WAIT_AUTH, /* peripheral: sent ACK, waiting AUTH */
    BL_SS_WAIT_AUTH_OK, /* central: sent AUTH, waiting OK */
    BL_SS_ESTABLISHED,
    BL_SS_FAILED,
    BL_SS_CLOSED,
} bl_session_state_t;

typedef struct {
    bl_role_t role;
    bl_session_state_t state;
    uint8_t proto_version; /* negotiated application proto (1) */

    uint8_t psk[BL_KEY_LEN];
    uint8_t eph_sk[32];
    uint8_t eph_pk[32];
    uint8_t peer_pk[32];
    uint8_t nonce_local[BL_NONCE_LEN];
    uint8_t nonce_peer[BL_NONCE_LEN];

    uint8_t tx_key[BL_KEY_LEN];
    uint8_t rx_key[BL_KEY_LEN];

    uint32_t tx_counter; /* next seq to send (post-handshake) */
    uint32_t rx_highest; /* highest accepted rx seq (strict increase) */
    bool keys_ready;

    /* failure accounting for lockout policy (enforced by caller) */
    uint16_t auth_failures;
} bl_session_t;

/* Randomness callback: fill buf with len cryptographically-random bytes.
 * On host/tests a deterministic source may be injected. */
typedef void (*bl_rng_fn)(void* ctx, uint8_t* buf, size_t len);

/*
 * Initialise a session with the provisioned pairing key. Generates the
 * ephemeral keypair and local nonce via rng. Returns BL_OK.
 */
int bl_session_init(
    bl_session_t* s,
    bl_role_t role,
    const uint8_t psk[BL_KEY_LEN],
    bl_rng_fn rng,
    void* rng_ctx);

/* Central only: produce the HELLO frame. */
int bl_session_start(bl_session_t* s, uint8_t* out, size_t out_cap);

/*
 * Feed one received frame buffer to the handshake/session. On a
 * handshake step that must emit a reply, writes it to out and sets
 * *out_len; otherwise *out_len = 0. Returns BL_OK on progress,
 * BL_E_* on protocol/crypto error (and moves to BL_SS_FAILED for
 * authentication failures).
 *
 * Once BL_SS_ESTABLISHED, data frames are NOT handled here; use
 * bl_session_open/bl_session_seal for payload traffic.
 */
int bl_session_handshake_rx(
    bl_session_t* s,
    const uint8_t* in,
    size_t in_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);

static inline bool bl_session_established(const bl_session_t* s) {
    return s && s->state == BL_SS_ESTABLISHED;
}

/*
 * Seal an application message: build an authenticated data frame with
 * the next tx counter. Returns encoded length or negative bl_result_t.
 */
int bl_session_seal(
    bl_session_t* s,
    uint8_t type,
    uint8_t flags,
    const uint8_t* payload,
    uint16_t payload_len,
    uint8_t* out,
    size_t out_cap);

/*
 * Open (verify + anti-replay) a received data frame. On success fills
 * *f with the decoded view (payload points into in). Enforces strictly
 * increasing rx seq. Returns BL_OK, BL_E_MAC, BL_E_REPLAY, BL_E_STATE.
 */
int bl_session_open(bl_session_t* s, const uint8_t* in, size_t in_len, bl_frame_t* f);

/* Wipe all key material. */
void bl_session_close(bl_session_t* s);

#ifdef __cplusplus
}
#endif
