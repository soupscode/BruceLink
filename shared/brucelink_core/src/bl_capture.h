/*
 * BruceLink shared core — capture session manager.
 *
 * Transport- and hardware-independent state machine for a packet-capture
 * session. The device supplies a backend vtable that actually drives the
 * radio and the filesystem (on the StickS3: esp_wifi promiscuous +
 * LittleFS; see docs/decisions and the Bruce audit). On the host the
 * tests inject a mock backend to exercise the lifecycle, counters and
 * failure handling without hardware.
 *
 * The manager owns: session state, selected mode/channel, live counters,
 * and the pcap bookkeeping. It never calls esp_wifi or touches files
 * directly.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_pcap.h"
#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BL_CAP_MODE_HANDSHAKE = 0, /* EAPOL 4-way only (StickS3, LittleFS) */
    BL_CAP_MODE_RAW = 1, /* full raw (needs SD on StickS3) */
    BL_CAP_MODE_DEAUTH_PCAP = 2, /* capture frames around an active test */
} bl_capture_mode_t;

typedef enum {
    BL_CAPST_IDLE = 0,
    BL_CAPST_RUNNING,
    BL_CAPST_STOPPING,
    BL_CAPST_ERROR,
} bl_capture_state_t;

typedef struct {
    uint32_t packets;
    uint32_t eapol;
    uint32_t handshakes;
    uint32_t beacons;
    uint32_t deauth;
    uint32_t dropped; /* queue overruns reported by backend */
    uint32_t bytes_written;
    uint32_t free_bytes; /* remaining capture storage */
} bl_capture_counts_t;

/* Backend contract. Each returns BL_OK or a negative bl_result_t.
 * `ctx` is the backend's own state. */
typedef struct {
    int (*start)(void* ctx, bl_capture_mode_t mode, uint8_t channel);
    int (*set_channel)(void* ctx, uint8_t channel);
    int (*poll)(void* ctx, bl_capture_counts_t* counts); /* refresh counters */
    int (*stop)(void* ctx); /* stop radio, flush+close file */
    /* returns 1 if the mode is runnable on this hardware, else 0 */
    int (*mode_available)(void* ctx, bl_capture_mode_t mode);
} bl_capture_backend_t;

typedef struct {
    const bl_capture_backend_t* be;
    void* be_ctx;
    bl_capture_state_t state;
    bl_capture_mode_t mode;
    uint8_t channel;
    uint32_t session_id;
    uint32_t start_ms;
    bl_capture_counts_t counts;
    int last_error;
} bl_capture_mgr_t;

void bl_capture_init(bl_capture_mgr_t* m, const bl_capture_backend_t* be, void* be_ctx);

/* Start a session. Validates channel (1..14) and that the backend
 * reports the mode available. Returns BL_OK, BL_E_BUSY, BL_E_ARG,
 * BL_E_DENIED (mode unavailable), or the backend error. */
int bl_capture_start(
    bl_capture_mgr_t* m,
    bl_capture_mode_t mode,
    uint8_t channel,
    uint32_t session_id,
    uint32_t now_ms);

/* Change channel mid-session (RUNNING only). */
int bl_capture_set_channel(bl_capture_mgr_t* m, uint8_t channel);

/* Refresh counters from the backend (RUNNING only). */
int bl_capture_poll(bl_capture_mgr_t* m);

/* Stop and flush. Idempotent when already IDLE. */
int bl_capture_stop(bl_capture_mgr_t* m);

static inline bool bl_capture_is_running(const bl_capture_mgr_t* m) {
    return m && m->state == BL_CAPST_RUNNING;
}

#ifdef __cplusplus
}
#endif
