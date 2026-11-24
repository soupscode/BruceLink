/*
 * BruceLink shared core — pairing (key agreement with code comparison).
 *
 * Gives two devices that share nothing a fresh 32-byte pairing key (the
 * PSK that bl_session authenticates with), with protection against a
 * man in the middle by having the user compare a 6-digit code shown on
 * both screens. Same idea as Bluetooth's "numeric comparison"; ADR 0005.
 *
 * I = initiator (the Flipper), R = responder (the StickS3). Both must be
 * put into pairing mode by their user; nothing here runs otherwise.
 *
 *   I->R PAIR_COMMIT  ver, commit = H("BLpair1 commit" | ver | pk_I | n_I),
 *                     name_I
 *   R->I PAIR_KEY     ver, pk_R, n_R, name_R
 *   I->R PAIR_REVEAL  pk_I, n_I                   (R checks the commit)
 *        both show    code = LE32(H("BLpair1 code" | T)[0..3]) mod 10^6
 *        user confirms the codes match, on each device
 *   I<>R PAIR_CONFIRM HMAC(psk, "BLpair1 confirm I|R" | T)   (after accept)
 *   I<>R PAIR_ABORT   reason                      (reject / cancel / timeout)
 *
 *   T   = H("BLpair1 transcript" | ver | lenI | name_I | pk_I | n_I
 *                                     | lenR | name_R | pk_R | n_R)
 *   psk = HKDF(salt = n_I | n_R, ikm = X25519(sk, pk_peer),
 *              info = "BLpair1 psk" | T, 32)
 *   key id (display only) = H("BLpair1 key id" | psk)[0..1]
 *
 * I commits to its key before seeing R's and R reveals its key before
 * seeing I's, so a man in the middle has to fix its own values on at
 * least one side before it can know the code that side will show: it
 * gets the two codes to agree with probability 1 in 10^6 per attempt, and
 * every attempt needs both users to accept.
 *
 * Pairing frames carry a zero MAC field, like the session handshake:
 * there is no shared key yet. Their integrity comes from the code
 * comparison and the confirm tags.
 *
 * The caller owns all transport I/O and the user interface; this module
 * only produces and consumes frame byte buffers. Allocation-free.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_frame.h"
#include "bl_session.h" /* bl_rng_fn */

#ifdef __cplusplus
extern "C" {
#endif

#define BL_PAIR_VERSION 1u
#define BL_PAIR_NAME_MAX 16u
#define BL_PAIR_CODE_MOD 1000000u /* 6 digits */
#define BL_PAIR_KEY_ID_LEN 2u
/* One pairing attempt, from the link coming up to both users accepting. */
#define BL_PAIR_TIMEOUT_MS 90000u
/* Every pairing frame fits in this many bytes. */
#define BL_PAIR_FRAME_MAX 128u

typedef enum {
    BL_PAIR_INITIATOR = 0, /* the Flipper */
    BL_PAIR_RESPONDER = 1, /* the StickS3 */
} bl_pair_role_t;

typedef enum {
    BL_PS_IDLE = 0, /* initiator: before bl_pair_start */
    BL_PS_WAIT_COMMIT, /* responder */
    BL_PS_WAIT_KEY, /* initiator */
    BL_PS_WAIT_REVEAL, /* responder */
    BL_PS_COMPARE, /* code ready: waiting for the local user */
    BL_PS_WAIT_PEER, /* local user accepted: waiting for the peer's confirm */
    BL_PS_DONE, /* both accepted, keys agree: bl_pair_result is valid */
    BL_PS_FAILED,
} bl_pair_state_t;

typedef enum {
    BL_PF_NONE = 0,
    BL_PF_REJECTED, /* the local user said the codes differ */
    BL_PF_PEER_REJECTED, /* the peer sent PAIR_ABORT (see peer_reason) */
    BL_PF_MISMATCH, /* commit or confirm tag wrong: interference or a bug */
    BL_PF_CRYPTO, /* peer key unusable (low-order point) */
    BL_PF_TIMEOUT,
    BL_PF_CANCELLED, /* the local side gave up (bl_pair_cancel) */
} bl_pair_fail_t;

/* PAIR_ABORT reason byte. */
typedef enum {
    BL_PAIR_ABORT_REJECTED = 1, /* user: codes differ */
    BL_PAIR_ABORT_CANCELLED = 2, /* user or app gave up */
    BL_PAIR_ABORT_TIMEOUT = 3,
    BL_PAIR_ABORT_MISMATCH = 4, /* commitment or key check failed on the sender's side */
} bl_pair_abort_t;

typedef struct {
    bl_pair_role_t role;
    bl_pair_state_t state;
    bl_pair_fail_t fail;
    uint8_t peer_reason; /* PAIR_ABORT reason, when fail == PEER_REJECTED */
    uint32_t started_ms;

    uint8_t sk[32];
    uint8_t pk[32];
    uint8_t n_local[BL_NONCE_LEN];
    uint8_t pk_peer[32];
    uint8_t n_peer[BL_NONCE_LEN];
    uint8_t commit_peer[32]; /* responder: the initiator's commitment */

    char name_local[BL_PAIR_NAME_MAX + 1];
    char name_peer[BL_PAIR_NAME_MAX + 1];
    uint8_t len_local;
    uint8_t len_peer;

    uint8_t transcript[32];
    uint8_t psk[BL_KEY_LEN];
    uint32_t code;
    bool peer_confirmed;
} bl_pair_t;

/*
 * Start a pairing attempt for one link. local_name is shown on the peer's
 * screen; it is cut to BL_PAIR_NAME_MAX bytes and anything outside
 * printable ASCII becomes '?'. Generates the key pair and nonce via rng.
 */
int bl_pair_init(
    bl_pair_t* p,
    bl_pair_role_t role,
    const char* local_name,
    bl_rng_fn rng,
    void* rng_ctx,
    uint32_t now_ms);

/* Initiator only: produce PAIR_COMMIT. Returns its length or BL_E_*. */
int bl_pair_start(bl_pair_t* p, uint8_t* out, size_t out_cap);

/*
 * Feed one received frame. A reply, if one is due, is written to out and
 * *out_len set (else 0).
 *   BL_OK        progress (check bl_pair_state)
 *   BL_E_STATE   a frame that is not the next step: ignored, nothing
 *                changed (e.g. a session HELLO from a peer that is not
 *                in pairing mode; the caller decides what to do with it)
 *   BL_E_FORMAT / BL_E_SHORT / ...   malformed: ignored, nothing changed
 *   BL_E_AUTH / BL_E_CRYPTO          the attempt failed (state FAILED)
 */
int bl_pair_rx(
    bl_pair_t* p,
    const uint8_t* in,
    size_t in_len,
    uint8_t* out,
    size_t out_cap,
    size_t* out_len);

/* The 6-digit code, once the state is COMPARE or later (else false). */
bool bl_pair_code(const bl_pair_t* p, uint32_t* code);

/*
 * The local user's answer, in COMPARE only. accept: sends PAIR_CONFIRM and
 * moves to WAIT_PEER, or straight to DONE if the peer's confirm already
 * arrived. !accept: sends PAIR_ABORT and fails with BL_PF_REJECTED.
 */
int bl_pair_accept(bl_pair_t* p, bool accept, uint8_t* out, size_t out_cap, size_t* out_len);

/* Give up (user backed out, link going away). Produces PAIR_ABORT if the
 * attempt was still running; *out_len = 0 otherwise. */
int bl_pair_cancel(bl_pair_t* p, uint8_t* out, size_t out_cap, size_t* out_len);

/* Call periodically. Returns true once when the attempt times out, with
 * PAIR_ABORT in out. */
bool bl_pair_tick(bl_pair_t* p, uint32_t now_ms, uint8_t* out, size_t out_cap, size_t* out_len);

static inline bl_pair_state_t bl_pair_state(const bl_pair_t* p) {
    return p ? p->state : BL_PS_FAILED;
}

/* Peer's display name (sanitised, NUL-terminated); "" before it is known. */
const char* bl_pair_peer_name(const bl_pair_t* p);

/* DONE only: copy out the pairing key and its display id. */
int bl_pair_result(
    const bl_pair_t* p,
    uint8_t psk[BL_KEY_LEN],
    uint8_t key_id[BL_PAIR_KEY_ID_LEN]);

/* Display id of a key (same derivation as the result's). */
void bl_pair_key_id(const uint8_t psk[BL_KEY_LEN], uint8_t key_id[BL_PAIR_KEY_ID_LEN]);

/* Wipe all secrets. */
void bl_pair_wipe(bl_pair_t* p);

#ifdef __cplusplus
}
#endif
