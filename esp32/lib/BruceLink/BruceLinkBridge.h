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
 * it compiles and is unit-tested off-device. NimBLE lives in the
 * transport layer; Bruce lives behind BruceBackend.
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

/* Implemented by the Bruce-dependent glue (src/modules/brucelink). The
 * bridge core calls only these; everything Bruce-specific stays here. */
class BruceBackend {
public:
    virtual ~BruceBackend() {}
    /* Fill out with a human-readable device/firmware summary; return
     * the number of bytes written (<= cap). */
    virtual size_t deviceInfo(char* out, size_t cap) = 0;
    /* Passive Wi-Fi scan rendered as text lines; return bytes written. */
    virtual size_t wifiScan(char* out, size_t cap) = 0;
    /* Provide the capture backend vtable + context for the manager. */
    virtual bool captureBackend(const bl_capture_backend_t** be, void** ctx) = 0;
    /* Forward an authorized active op (e.g. deauth) to Bruce. target is
     * a 6-byte BSSID. Returns frames transmitted, or <0 on error. The
     * bridge has already checked the safety gate. */
    virtual int activeOp(uint16_t cap_id, const uint8_t* target, uint8_t channel) = 0;
    /* Monotonic milliseconds (millis()). */
    virtual uint32_t nowMs() = 0;
    /* Fill 32 random bytes (esp_fill_random). */
    virtual void randomBytes(uint8_t* out, size_t len) = 0;
};

/* tx callback: send one chunk (<= mtu) to the connected central. */
typedef void (*TxChunkFn)(void* ctx, const uint8_t* chunk, size_t len);

class BruceLinkBridge {
public:
    BruceLinkBridge();
    /* Provision the pairing key and operator secret, wire the backend
     * and the tx path. mtu is the usable GATT payload per chunk. */
    void begin(
        const uint8_t psk[BL_KEY_LEN],
        BruceBackend* backend,
        TxChunkFn tx,
        void* tx_ctx,
        size_t mtu);

    /* Call when a BLE link is established (resets session + reassembly). */
    void onConnect();
    /* Call when the BLE link drops: fail-safe (e-stop, stop capture). */
    void onDisconnect();

    /* Feed one received GATT chunk (one write/notify). */
    void onChunk(const uint8_t* chunk, size_t len);

    /* Poll capture counters so BL_MSG_STATUS reflects live numbers. */
    void tick();

    bool established() const {
        return bl_session_established(&session_);
    }

private:
    void onFrame(const uint8_t* frame, size_t len);
    void handleRequest(const bl_frame_t* f);
    void sendSealed(uint8_t type, uint8_t flags, const uint8_t* pl, uint16_t len);
    void sendRaw(const uint8_t* frame, size_t len); /* fragment + tx */
    void sendError(uint16_t err);
    void sendStatus();

    bl_session_t session_;
    bl_safety_t safety_;
    bl_capture_mgr_t capture_;
    bl_reasm_t reasm_;
    BruceBackend* backend_;
    TxChunkFn tx_;
    void* tx_ctx_;
    size_t mtu_;
    bool link_up_;
};

} // namespace brucelink
