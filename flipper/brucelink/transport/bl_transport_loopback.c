/*
 * BruceLink Flipper app — in-process loopback transport.
 *
 * Runs a minimal mock StickS3 "peripheral" in the same process so the
 * app's connection flow, session handshake and request/response paths
 * can be exercised on a Flipper with no StickS3 present (a demo/dev
 * aid, selectable in Settings). The mock answers PING, DEVICE_INFO,
 * CAPS and a canned Wi-Fi scan. It shares the real protocol core, so
 * what works here is the genuine wire protocol — only the radio is
 * absent.
 * SPDX-License-Identifier: MIT
 */
#include "bl_transport.h"

#include "../brucelink_i.h" /* BL_HS_FRAME_MAX */
#include "../../../shared/brucelink_core/src/bl_capability.h"
#include "../../../shared/brucelink_core/src/bl_session.h"

#include <furi.h>
#include <furi_hal_random.h>
#include <string.h>

/* The demo pairing key. On real hardware the key is provisioned per
 * device; this constant only pairs the app with its built-in mock. */
static const uint8_t LOOPBACK_PSK[BL_KEY_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
    0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};

#define RXQ 4

typedef struct {
    bl_session_t peer; /* the mock peripheral session */
    bool connected;
    /* frames queued from peripheral back to the app */
    uint8_t q[RXQ][BL_MAX_MSG];
    size_t qlen[RXQ];
    uint8_t qhead, qtail, qcount;
} Loopback;

static const bl_capability_desc_t MOCK_CAPS[] = {
    {BL_CAP_DEVICE_INFO, "device.info", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_WIFI_SCAN, "wifi.scan", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_SNIFF_HANDSHAKE, "wifi.sniff.handshake", BL_OP_PASSIVE, BL_CAP_F_HW_UNVERIFIED, 1},
};

static void loopback_rng(void* ctx, uint8_t* buf, size_t len) {
    UNUSED(ctx);
    furi_hal_random_fill_buf(buf, len);
}

static void q_push(Loopback* l, const uint8_t* f, size_t n) {
    if(l->qcount >= RXQ || n > BL_MAX_MSG) return;
    memcpy(l->q[l->qtail], f, n);
    l->qlen[l->qtail] = n;
    l->qtail = (uint8_t)((l->qtail + 1) % RXQ);
    l->qcount++;
}

/* Build and queue a sealed response from the mock peripheral. */
static void mock_respond(Loopback* l, uint8_t type, const uint8_t* pl, uint16_t len) {
    uint8_t out[BL_MAX_MSG];
    int n = bl_session_seal(&l->peer, type, BL_FLAG_RESPONSE, pl, len, out, sizeof(out));
    if(n > 0) q_push(l, out, (size_t)n);
}

/* Handle one established-session request from the app. */
static void mock_handle_request(Loopback* l, const bl_frame_t* f) {
    switch(f->type) {
    case BL_MSG_PING:
        mock_respond(l, BL_MSG_PONG, f->payload, f->payload_len);
        break;
    case BL_MSG_DEVICE_INFO_REQ: {
        const char* info = "BruceLink mock\nBruce 1.17 (demo)\nStickS3 (simulated)";
        mock_respond(l, BL_MSG_DEVICE_INFO, (const uint8_t*)info, (uint16_t)strlen(info));
        break;
    }
    case BL_MSG_CAPS_REQ: {
        bl_capability_registry_t reg = {MOCK_CAPS, sizeof(MOCK_CAPS) / sizeof(MOCK_CAPS[0])};
        uint8_t buf[BL_MAX_PAYLOAD];
        int cn = bl_caps_serialize(&reg, buf, sizeof(buf));
        if(cn > 0) mock_respond(l, BL_MSG_CAPS, buf, (uint16_t)cn);
        break;
    }
    case BL_MSG_CMD_REQ: {
        /* demo Wi-Fi scan result (two canned APs), newline separated */
        const char* res = "2 networks (demo)\nHomeNet  -52dBm ch6\nCafe_Guest -70dBm ch1";
        mock_respond(l, BL_MSG_CMD_RESP, (const uint8_t*)res, (uint16_t)strlen(res));
        break;
    }
    default: {
        uint8_t err[2] = {BL_ERR_UNKNOWN_TYPE, 0};
        mock_respond(l, BL_MSG_ERROR, err, sizeof(err));
        break;
    }
    }
}

static bool lb_connect(BlTransport* t) {
    Loopback* l = t->impl;
    /* (re)initialise the mock peripheral session */
    if(bl_session_init(&l->peer, BL_ROLE_PERIPHERAL, LOOPBACK_PSK, loopback_rng, NULL) != BL_OK)
        return false;
    l->qhead = l->qtail = l->qcount = 0;
    l->connected = true;
    return true;
}

static void lb_disconnect(BlTransport* t) {
    Loopback* l = t->impl;
    bl_session_close(&l->peer);
    l->connected = false;
    l->qhead = l->qtail = l->qcount = 0;
}

static int lb_send(BlTransport* t, const uint8_t* frame, size_t len) {
    Loopback* l = t->impl;
    if(!l->connected) return BL_E_STATE;
    if(!bl_session_established(&l->peer)) {
        /* drive the handshake on the peripheral side; the AUTH_OK reply
         * is a sealed frame with no payload, so BL_HS_FRAME_MAX suffices */
        uint8_t out[BL_HS_FRAME_MAX];
        size_t olen = 0;
        int rc = bl_session_handshake_rx(&l->peer, frame, len, out, sizeof(out), &olen);
        if(rc != BL_OK) return rc;
        if(olen) q_push(l, out, olen);
        return BL_OK;
    }
    bl_frame_t f;
    int rc = bl_session_open(&l->peer, frame, len, &f);
    if(rc != BL_OK) return rc;
    mock_handle_request(l, &f);
    return BL_OK;
}

static void lb_poll(BlTransport* t, BlTransportRxCb cb, void* ctx) {
    Loopback* l = t->impl;
    while(l->qcount > 0) {
        const uint8_t* f = l->q[l->qhead];
        size_t n = l->qlen[l->qhead];
        l->qhead = (uint8_t)((l->qhead + 1) % RXQ);
        l->qcount--;
        cb(f, n, ctx);
    }
}

static bool lb_is_connected(BlTransport* t) {
    Loopback* l = t->impl;
    return l->connected;
}

static const BlTransportApi LOOPBACK_API = {
    .name = "Loopback (demo)",
    .connect = lb_connect,
    .disconnect = lb_disconnect,
    .send = lb_send,
    .poll = lb_poll,
    .is_connected = lb_is_connected,
};

BlTransport* bl_transport_loopback_alloc(void) {
    BlTransport* t = malloc(sizeof(BlTransport));
    Loopback* l = malloc(sizeof(Loopback));
    memset(l, 0, sizeof(*l));
    t->api = &LOOPBACK_API;
    t->impl = l;
    return t;
}

void bl_transport_loopback_free(BlTransport* t) {
    if(!t) return;
    free(t->impl);
    free(t);
}
