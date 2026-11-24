/*
 * BruceLink shared core — session initiator ("client") state machine.
 * SPDX-License-Identifier: MIT
 */
#include "bl_client.h"

#include <string.h>

void bl_client_init(bl_client_t* c, const uint8_t psk[BL_KEY_LEN], bl_rng_fn rng, void* rng_ctx) {
    if(c == NULL) return;
    memset(c, 0, sizeof(*c));
    if(psk != NULL) memcpy(c->psk, psk, BL_KEY_LEN);
    c->rng = rng;
    c->rng_ctx = rng_ctx;
    c->state = BL_CL_IDLE;
    c->hs_timeout_ms = BL_CLIENT_HS_TIMEOUT_MS;
    c->req_timeout_ms = BL_CLIENT_REQ_TIMEOUT_MS;
    c->last_error = BL_OK;
}

int bl_client_link_up(bl_client_t* c, uint32_t now_ms, uint8_t* out, size_t out_cap) {
    if(c == NULL || out == NULL || c->rng == NULL) return BL_E_ARG;
    bl_session_close(&c->session); /* discard any previous link's secrets */
    c->req_pending = false;
    c->last_error = BL_OK;
    int rc = bl_session_init(&c->session, BL_ROLE_CENTRAL, c->psk, c->rng, c->rng_ctx);
    if(rc != BL_OK) {
        c->state = BL_CL_FAILED;
        c->last_error = rc;
        return rc;
    }
    int n = bl_session_start(&c->session, out, out_cap);
    if(n < 0) {
        c->state = BL_CL_FAILED;
        c->last_error = n;
        return n;
    }
    c->hs_start_ms = now_ms;
    c->state = BL_CL_HANDSHAKING;
    return n;
}

void bl_client_link_down(bl_client_t* c) {
    if(c == NULL) return;
    bl_session_close(&c->session);
    c->req_pending = false;
    c->state = BL_CL_IDLE;
}

int bl_client_rx(
    bl_client_t* c,
    const uint8_t* in,
    size_t in_len,
    uint8_t* reply,
    size_t reply_cap,
    size_t* reply_len,
    bl_frame_t* msg,
    bool* have_msg) {
    if(c == NULL || in == NULL || reply_len == NULL || msg == NULL || have_msg == NULL)
        return BL_E_ARG;
    *reply_len = 0;
    *have_msg = false;

    if(c->state == BL_CL_HANDSHAKING) {
        int rc = bl_session_handshake_rx(&c->session, in, in_len, reply, reply_cap, reply_len);
        if(rc != BL_OK) {
            *reply_len = 0;
            /* Terminal only if the session itself gave up (bad proof,
             * no common version). Noise, a truncated frame or a frame of
             * the wrong kind is ignored: the real reply may still come,
             * and if it does not the handshake timeout ends the attempt. */
            if(c->session.state == BL_SS_FAILED) {
                c->state = BL_CL_FAILED;
                c->last_error = rc;
            }
            return rc;
        }
        if(bl_session_established(&c->session)) c->state = BL_CL_READY;
        return BL_OK;
    }
    if(c->state != BL_CL_READY) return BL_E_STATE;

    int rc = bl_session_open(&c->session, in, in_len, msg);
    if(rc != BL_OK) return rc; /* forged, replayed or corrupt: ignore it */
    if(msg->flags & BL_FLAG_RESPONSE) {
        if(!c->req_pending || msg->tag != c->req_tag) {
            c->stale_responses++; /* late answer to an earlier request */
            return BL_OK;
        }
        c->req_pending = false;
    }
    *have_msg = true;
    return BL_OK;
}

int bl_client_request(
    bl_client_t* c,
    uint8_t type,
    const uint8_t* payload,
    uint16_t payload_len,
    uint32_t now_ms,
    uint8_t* out,
    size_t out_cap) {
    if(c == NULL || out == NULL) return BL_E_ARG;
    if(c->state != BL_CL_READY) return BL_E_STATE;
    if(c->req_pending) return BL_E_BUSY;
    /* 1..255, then back to 1: 0 means "no tag" */
    const uint8_t tag = (c->next_tag >= 255u) ? 1u : (uint8_t)(c->next_tag + 1u);
    int n = bl_session_seal_tag(&c->session, type, BL_FLAG_NONE, tag, payload, payload_len, out, out_cap);
    if(n < 0) return n;
    c->next_tag = tag;
    c->req_tag = tag;
    c->req_pending = true;
    c->req_start_ms = now_ms;
    return n;
}

bl_client_event_t bl_client_tick(bl_client_t* c, uint32_t now_ms) {
    if(c == NULL) return BL_CL_EVT_NONE;
    /* unsigned subtraction is wrap-safe across tick rollover */
    if(c->state == BL_CL_HANDSHAKING && (uint32_t)(now_ms - c->hs_start_ms) >= c->hs_timeout_ms) {
        c->state = BL_CL_FAILED;
        c->last_error = BL_E_STATE;
        return BL_CL_EVT_HANDSHAKE_TIMEOUT;
    }
    if(c->state == BL_CL_READY && c->req_pending &&
       (uint32_t)(now_ms - c->req_start_ms) >= c->req_timeout_ms) {
        c->req_pending = false;
        return BL_CL_EVT_REQUEST_TIMEOUT;
    }
    return BL_CL_EVT_NONE;
}
