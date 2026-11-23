/*
 * BruceLink ESP32-S3 bridge — transport- and firmware-independent core.
 *
 * Holds the authenticated session (peripheral role: it answers the
 * Flipper's handshake), the remote-operation safety gate, the capture
 * session manager and the reassembly buffer. It turns received GATT
 * chunks into protocol frames, dispatches them to a BruceBackend (the
 * only place that touches real Bruce functions), and emits response
 * chunks through a tx callback.
 *
 * This file depends ONLY on the shared protocol core (portable C99), so
 * it compiles and is unit-tested on the host (tests/c/test_bridge.cpp).
 * NimBLE lives in the transport layer; Bruce lives behind BruceBackend.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "bl_capability.h"
#include "bl_capture.h"
#include "bl_reasm.h"
#include "bl_safety.h"
#include "bl_session.h"
}

namespace brucelink {

/* Reserved capability ids for bridge control (not Bruce functions). */
static const uint16_t kCtrlEnableActive = 0xFF01;

/* Upper bound on frames a single active op may transmit. The bridge
 * authorizes min(this, remaining window budget) and passes it to the
 * backend as a hard cap; unused frames are refunded to the window. */
static const uint32_t kMaxFramesPerActiveOp = 16;

/* A new link must complete the authenticated handshake within this time,
 * or the transport drops it. Requires the Flipper to start the handshake
 * as soon as the BLE link is up (ADR 0001). */
static const uint32_t kAuthTimeoutMs = 10000;

/* Implemented by the Bruce-dependent glue (src/modules/brucelink). The
 * bridge core calls only these; everything Bruce-specific stays there. */
class BruceBackend {
public:
    virtual ~BruceBackend() {}
    /* Write a human-readable device/firmware summary into out (text, NUL-
     * terminated) and return its length (< cap). The bridge clamps the
     * reported length to the text actually present. */
    virtual size_t deviceInfo(char* out, size_t cap) = 0;
    /* Passive Wi-Fi scan rendered as text lines; same contract. */
    virtual size_t wifiScan(char* out, size_t cap) = 0;
    /* Provide the capture backend vtable + context; false if this build has
     * no capture support (capture requests then report "disabled"). */
    virtual bool captureBackend(const bl_capture_backend_t** be, void** ctx) = 0;
    /* Forward an authorized active op (e.g. deauth) to Bruce. target is a
     * specific 6-byte BSSID. The backend MUST transmit at most max_frames
     * frames and returns the number actually transmitted, or <0 on error.
     * Returning more than max_frames is a contract violation: the bridge
     * treats it as a fault and engages the emergency stop. The bridge has
     * already passed the safety gate before calling. */
    virtual int activeOp(uint16_t cap_id, const uint8_t* target, uint8_t channel, uint32_t max_frames) = 0;
    /* Monotonic milliseconds (millis()). */
    virtual uint32_t nowMs() = 0;
    /* Fill len cryptographically random bytes (esp_fill_random). */
    virtual void randomBytes(uint8_t* out, size_t len) = 0;
};

/* tx callback: send one chunk (<= mtu) to the connected central. */
typedef void (*TxChunkFn)(void* ctx, const uint8_t* chunk, size_t len);

class BruceLinkBridge {
public:
    BruceLinkBridge();
    ~BruceLinkBridge();
    /* Provision the pairing key, wire the backend and the tx path. mtu is
     * the usable GATT payload per chunk. The key is kept for the bridge's
     * lifetime and reused for every (re)connection. */
    void begin(
        const uint8_t psk[BL_KEY_LEN],
        BruceBackend* backend,
        TxChunkFn tx,
        void* tx_ctx,
        size_t mtu);

    /* Call when a BLE link is established: fresh session from the
     * provisioned key, cleared reassembly, safety gate reset (e-stop
     * cleared, active ops disabled until re-enabled). */
    void onConnect();
    /* Call when the BLE link drops: fail-safe (e-stop, stop capture),
     * then discard session secrets. The provisioned key is kept. */
    void onDisconnect();

    /* Update the usable per-chunk payload after ATT MTU negotiation. */
    void setMtu(size_t mtu);

    /* Feed one received GATT chunk (one write/notify). */
    void onChunk(const uint8_t* chunk, size_t len);

    /* Poll capture counters; a backend failure stops the capture. */
    void tick();

    /* Link supervision. True when the transport should drop the current
     * link: it is up but has no authenticated session, and the handshake
     * either failed terminally (bad proof, version mismatch, unprovisioned
     * bridge) or did not complete within kAuthTimeoutMs of onConnect().
     * Transport connectivity alone never holds the link. Poll from the
     * transport loop; an established session never triggers it. */
    bool needsDisconnect();

    bool established() const {
        return bl_session_established(&session_);
    }

private:
    BruceLinkBridge(const BruceLinkBridge&);
    BruceLinkBridge& operator=(const BruceLinkBridge&);

    void onFrame(const uint8_t* frame, size_t len);
    void handleRequest(const bl_frame_t* f);
    void handleCommand(const bl_frame_t* f);
    void handleActiveOp(const bl_capability_desc_t* d, const uint8_t* args, uint16_t arglen);
    void handleCaptureStart(const bl_frame_t* f);
    bool radioAllowed(); /* passive radio use permitted (not e-stopped) */
    void sendSealed(uint8_t type, uint8_t flags, const uint8_t* pl, uint16_t len);
    void sendText(uint8_t type, const char* text, size_t len);
    void sendRaw(const uint8_t* frame, size_t len); /* fragment + tx */
    void sendError(uint16_t err);
    void sendStatus();

    uint8_t psk_[BL_KEY_LEN]; /* provisioned pairing key (long-lived) */
    bool have_psk_;
    bl_session_t session_; /* per-connection; secrets wiped on disconnect */
    bl_safety_t safety_;
    bl_capture_mgr_t capture_;
    bl_reasm_t reasm_;
    BruceBackend* backend_;
    TxChunkFn tx_;
    void* tx_ctx_;
    size_t mtu_;
    bool link_up_;
    uint32_t link_up_ms_; /* backend time of the last onConnect() */
};

} // namespace brucelink
