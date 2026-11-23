/*
 * BruceLink Flipper app — transport abstraction.
 *
 * The app talks to the protocol core through this interface, so the UI
 * and session logic are independent of whether frames travel over BLE or
 * an in-process loopback. A transport delivers and accepts whole protocol
 * frames; fragmentation is its own concern (bl_reasm).
 *
 * Lifecycle: start() brings the transport up (BLE: start the GATT profile
 * and advertise; loopback: nothing to wait for). A *link* then comes and
 * goes on its own: link_id() is 0 while no peer can receive, and a
 * different non-zero value for every new link, so a drop-and-reconnect
 * between two polls is never mistaken for the same link. All calls are
 * made from the app thread.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../../shared/brucelink_core/src/bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BlTransport BlTransport;

/* Called for each complete frame received from the peer. */
typedef void (*BlTransportRxCb)(const uint8_t* frame, size_t len, void* ctx);

typedef struct {
    const char* name;
    /* Bring the transport up. False if it cannot start (e.g. BLE refused). */
    bool (*start)(BlTransport* t);
    /* Tear it down and release the radio. Safe if start() failed. */
    void (*stop)(BlTransport* t);
    /* 0 = no link; otherwise an id unique to the current link. */
    uint32_t (*link_id)(BlTransport* t);
    /* Force the current link down (e.g. handshake timed out); the
     * transport stays started and waits for a new link. */
    void (*drop_link)(BlTransport* t);
    /* Send one complete frame. Returns BL_OK or a negative bl_result_t. */
    int (*send)(BlTransport* t, const uint8_t* frame, size_t len);
    /* Deliver any received frames via cb(ctx). Non-blocking. */
    void (*poll)(BlTransport* t, BlTransportRxCb cb, void* ctx);
    /* One short line for the status screen (e.g. "advertising"). */
    const char* (*status)(BlTransport* t);
    /* Optional (may be NULL): write a short NUL-terminated line of link
     * counters for troubleshooting into out[cap]. */
    void (*diag)(BlTransport* t, char* out, size_t cap);
    void (*free)(BlTransport* t);
} BlTransportApi;

struct BlTransport {
    const BlTransportApi* api;
    void* impl;
};

/* In-process loopback: a mock StickS3 running the real protocol, so the
 * app is exercisable without hardware. */
BlTransport* bl_transport_loopback_alloc(void);
/* BLE: the Flipper as a GATT peripheral for the StickS3 (ADR 0001). */
BlTransport* bl_transport_ble_alloc(void);

#ifdef __cplusplus
}
#endif
