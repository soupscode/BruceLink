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

#include "../../../shared/brucelink_core/src/bl_capability.h"
#include "../../../shared/brucelink_core/src/bl_client.h" /* BL_CLIENT_HS_FRAME_MAX */
#include "../../../shared/brucelink_core/src/bl_session.h"
#include "../../../shared/brucelink_core/src/bl_demo_key.h"

#include <furi.h>
#include <furi_hal_random.h>
#include <string.h>

#define RXQ 4

typedef struct {
    bl_session_t peer; /* the mock peripheral session */
    bool started;
    uint32_t link; /* 0 = down; bumped for every new link */
    uint32_t next_link;
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

/* (Re)create the mock peer and present it as a brand-new link. */
static bool lb_new_link(Loopback* l) {
    bl_session_close(&l->peer);
    l->qhead = l->qtail = l->qcount = 0;
    if(bl_session_init(&l->peer, BL_ROLE_PERIPHERAL, BRUCELINK_DEMO_PSK, loopback_rng, NULL) !=
       BL_OK) {
        l->link = 0;
        return false;
    }
    l->link = ++l->next_link;
    if(l->link == 0) l->link = ++l->next_link; /* 0 is reserved for "down" */
    return true;
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

static bool lb_start(BlTransport* t) {
    Loopback* l = t->impl;
    l->started = lb_new_link(l);
    return l->started;
}

static void lb_stop(BlTransport* t) {
    Loopback* l = t->impl;
    bl_session_close(&l->peer);
    l->started = false;
    l->link = 0;
    l->qhead = l->qtail = l->qcount = 0;
}

static uint32_t lb_link_id(BlTransport* t) {
    Loopback* l = t->impl;
    return l->started ? l->link : 0;
}

static void lb_drop_link(BlTransport* t) {
    Loopback* l = t->impl;
    if(l->started) lb_new_link(l); /* the mock "reconnects" at once */
}

static int lb_send(BlTransport* t, const uint8_t* frame, size_t len) {
    Loopback* l = t->impl;
    if(!l->started || l->link == 0) return BL_E_STATE;
    if(!bl_session_established(&l->peer)) {
        /* drive the handshake on the peripheral side; every handshake
         * frame, incl. the payload-less AUTH_OK, fits the small buffer */
        uint8_t out[BL_CLIENT_HS_FRAME_MAX];
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

static const char* lb_status(BlTransport* t) {
    Loopback* l = t->impl;
    return l->started ? "loopback (built-in mock)" : "stopped";
}

static void lb_free(BlTransport* t) {
    if(!t) return;
    lb_stop(t);
    free(t->impl);
    free(t);
}

static const BlTransportApi LOOPBACK_API = {
    .name = "Loopback",
    .start = lb_start,
    .stop = lb_stop,
    .link_id = lb_link_id,
    .drop_link = lb_drop_link,
    .send = lb_send,
    .poll = lb_poll,
    .status = lb_status,
    .free = lb_free,
};

BlTransport* bl_transport_loopback_alloc(void) {
    BlTransport* t = malloc(sizeof(BlTransport));
    Loopback* l = malloc(sizeof(Loopback));
    memset(l, 0, sizeof(*l));
    t->api = &LOOPBACK_API;
    t->impl = l;
    return t;
}
