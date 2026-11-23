/*
 * BruceLink shared core — session initiator ("client") state machine.
 *
 * The Flipper side of a link: starts the handshake the moment the
 * transport reports the peer is ready, tracks one outstanding request,
 * and turns silence into explicit timeout events. Pure logic over
 * bl_session with caller-supplied time, so it is host-tested (including
 * end-to-end against the ESP32 bridge core) and the UI stays thin.
 *
 * Timeouts and recovery
 *   - Handshake: must complete within hs_timeout_ms of link-up
 *     (default 5 s). On expiry the client reports
 *     BL_CL_EVT_HANDSHAKE_TIMEOUT once and enters FAILED; the caller
 *     should drop the transport link and wait for a new one. There is no
 *     in-place retransmit: the responder cannot rewind a half-finished
 *     handshake, and BLE delivers GATT writes/notifications reliably, so
 *     a clean new link is both simpler and correct. 5 s leaves room for
 *     the two X25519 operations the Flipper performs inside the window
 *     (their duration on the device is not yet measured) and is still
 *     inside the StickS3's own 10 s authentication deadline (ADR 0001 §6).
 *     A frame that is not a valid next handshake step is ignored, not
 *     fatal; only the session failing (bad proof, no common version) is.
 *   - Request: one at a time; no response within req_timeout_ms
 *     (default 10 s: the StickS3 answers a Wi-Fi scan only after the scan
 *     has run) reports BL_CL_EVT_REQUEST_TIMEOUT once and frees the slot.
 *     The session stays up.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_CLIENT_HS_TIMEOUT_MS 5000u
#define BL_CLIENT_REQ_TIMEOUT_MS 10000u
/* Largest handshake frame: header + HELLO_ACK payload (81) + MAC = 109. */
#define BL_CLIENT_HS_FRAME_MAX 128u

typedef enum {
    BL_CL_IDLE = 0, /* no link */
    BL_CL_HANDSHAKING,
    BL_CL_READY, /* authenticated session established */
    BL_CL_FAILED, /* handshake failed or timed out; needs a new link */
} bl_client_state_t;

typedef enum {
    BL_CL_EVT_NONE = 0,
    BL_CL_EVT_HANDSHAKE_TIMEOUT,
    BL_CL_EVT_REQUEST_TIMEOUT,
} bl_client_event_t;

typedef struct {
    bl_session_t session;
    uint8_t psk[BL_KEY_LEN];
    bl_rng_fn rng;
    void* rng_ctx;
    bl_client_state_t state;
    uint32_t hs_timeout_ms;
    uint32_t req_timeout_ms;
    uint32_t hs_start_ms;
    uint32_t req_start_ms;
    bool req_pending;
    int last_error; /* bl_result_t of the last handshake failure */
} bl_client_t;

/* Store the pairing key and RNG. No link yet (IDLE). */
void bl_client_init(bl_client_t* c, const uint8_t psk[BL_KEY_LEN], bl_rng_fn rng, void* rng_ctx);

/*
 * The transport link is up and the peer can receive: start a fresh
 * session and write the HELLO frame to out (cap >= BL_CLIENT_HS_FRAME_MAX).
 * Returns the frame length or a negative bl_result_t. Call it immediately:
 * the responder enforces an authentication deadline.
 */
int bl_client_link_up(bl_client_t* c, uint32_t now_ms, uint8_t* out, size_t out_cap);

/* The transport link dropped: wipe session secrets, back to IDLE. */
void bl_client_link_down(bl_client_t* c);

/*
 * Feed one received frame.
 *  - During the handshake: may write a reply to `reply` and set *reply_len
 *    (send it); a failure moves to FAILED and returns the error.
 *  - Once READY: verifies and anti-replays the frame; on success sets
 *    *have_msg and fills *msg (payload points into `in`). A response frame
 *    releases the pending-request slot.
 * reply_cap must be >= BL_CLIENT_HS_FRAME_MAX.
 */
int bl_client_rx(
    bl_client_t* c,
    const uint8_t* in,
    size_t in_len,
    uint8_t* reply,
    size_t reply_cap,
    size_t* reply_len,
    bl_frame_t* msg,
    bool* have_msg);

/*
 * Seal a request into out. Returns its length, BL_E_STATE if not READY,
 * or BL_E_BUSY while another request is still awaiting its response.
 */
int bl_client_request(
    bl_client_t* c,
    uint8_t type,
    const uint8_t* payload,
    uint16_t payload_len,
    uint32_t now_ms,
    uint8_t* out,
    size_t out_cap);

/* Advance time. Each timeout is reported exactly once. */
bl_client_event_t bl_client_tick(bl_client_t* c, uint32_t now_ms);

static inline bool bl_client_ready(const bl_client_t* c) {
    return c && c->state == BL_CL_READY;
}

#ifdef __cplusplus
}
#endif
