/*
 * BruceLink Flipper app — transport abstraction.
 *
 * The app talks to the protocol core through this interface, so the UI
 * and session logic are independent of whether frames travel over BLE,
 * a serial line, or an in-process loopback. Each transport delivers and
 * accepts whole protocol frames; fragmentation for a byte- or MTU-
 * limited link is the transport's own concern (see bl_reasm/bl_cobs).
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
    bool (*connect)(BlTransport* t);
    void (*disconnect)(BlTransport* t);
    /* Queue one complete frame for transmission. Returns BL_OK or <0. */
    int (*send)(BlTransport* t, const uint8_t* frame, size_t len);
    /* Deliver any received frames via cb(ctx). Non-blocking. */
    void (*poll)(BlTransport* t, BlTransportRxCb cb, void* ctx);
    bool (*is_connected)(BlTransport* t);
} BlTransportApi;

struct BlTransport {
    const BlTransportApi* api;
    void* impl;
};

static inline bool bl_transport_connect(BlTransport* t) {
    return t && t->api->connect(t);
}
static inline void bl_transport_disconnect(BlTransport* t) {
    if(t) t->api->disconnect(t);
}
static inline int bl_transport_send(BlTransport* t, const uint8_t* f, size_t n) {
    return t ? t->api->send(t, f, n) : BL_E_ARG;
}
static inline void bl_transport_poll(BlTransport* t, BlTransportRxCb cb, void* ctx) {
    if(t) t->api->poll(t, cb, ctx);
}
static inline bool bl_transport_is_connected(BlTransport* t) {
    return t && t->api->is_connected(t);
}

/* Factory for the in-process loopback transport (runs a mock StickS3
 * peripheral so the app is exercisable without hardware). */
BlTransport* bl_transport_loopback_alloc(void);
void bl_transport_loopback_free(BlTransport* t);

#ifdef __cplusplus
}
#endif
