/*
 * BruceLink ESP32-S3 bridge core.
 * SPDX-License-Identifier: MIT
 */
#include "BruceLinkBridge.h"

#include <stdio.h>
#include <string.h>

namespace brucelink {

/* Capabilities this bridge advertises. The Flipper renders its menus
 * from this, so it never offers a function the StickS3 build lacks.
 * HW_UNVERIFIED until a hardware session confirms each path. */
static const bl_capability_desc_t kCaps[] = {
    {BL_CAP_DEVICE_INFO, "device.info", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_STATUS, "status", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_WIFI_SCAN, "wifi.scan", BL_OP_PASSIVE, BL_CAP_F_HW_UNVERIFIED, 1},
    {BL_CAP_SNIFF_HANDSHAKE,
     "wifi.sniff.handshake",
     BL_OP_PASSIVE,
     BL_CAP_F_HW_UNVERIFIED,
     1},
    {BL_CAP_SNIFF_RAW,
     "wifi.sniff.raw",
     BL_OP_PASSIVE,
     BL_CAP_F_REQUIRES_EXT_HW | BL_CAP_F_HW_UNVERIFIED,
     1},
    {BL_CAP_DEAUTH,
     "wifi.deauth",
     BL_OP_ACTIVE,
     BL_CAP_F_ACTIVE_RF | BL_CAP_F_REQUIRES_TARGET | BL_CAP_F_HW_UNVERIFIED,
     1},
};
static const bl_capability_registry_t kRegistry = {kCaps, sizeof(kCaps) / sizeof(kCaps[0])};

static void bridge_rng(void* ctx, uint8_t* buf, size_t len) {
    BruceBackend* b = static_cast<BruceBackend*>(ctx);
    b->randomBytes(buf, len);
}

BruceLinkBridge::BruceLinkBridge()
    : backend_(nullptr), tx_(nullptr), tx_ctx_(nullptr), mtu_(20), link_up_(false) {
    memset(&session_, 0, sizeof(session_));
    bl_safety_init(&safety_);
    bl_reasm_init(&reasm_);
}

void BruceLinkBridge::begin(
    const uint8_t psk[BL_KEY_LEN],
    BruceBackend* backend,
    TxChunkFn tx,
    void* tx_ctx,
    size_t mtu) {
    backend_ = backend;
    tx_ = tx;
    tx_ctx_ = tx_ctx;
    mtu_ = mtu < BL_MIN_CHUNK ? BL_MIN_CHUNK : mtu;
    bl_safety_init(&safety_);
    const bl_capture_backend_t* cbe = nullptr;
    void* cctx = nullptr;
    if(backend_->captureBackend(&cbe, &cctx)) bl_capture_init(&capture_, cbe, cctx);
    /* stash psk for (re)init on each connect */
    memcpy(session_.psk, psk, BL_KEY_LEN); /* used by onConnect reinit */
}

void BruceLinkBridge::onConnect() {
    uint8_t psk[BL_KEY_LEN];
    memcpy(psk, session_.psk, BL_KEY_LEN);
    bl_session_init(&session_, BL_ROLE_PERIPHERAL, psk, bridge_rng, backend_);
    bl_reasm_init(&reasm_);
    bl_safety_reset(&safety_); /* clears any prior e-stop for the new link */
    link_up_ = true;
}

void BruceLinkBridge::onDisconnect() {
    link_up_ = false;
    bl_safety_on_link_loss(&safety_); /* fail-safe: halt active ops */
    if(bl_capture_is_running(&capture_)) bl_capture_stop(&capture_);
    bl_session_close(&session_);
    bl_reasm_init(&reasm_);
}

void BruceLinkBridge::onChunk(const uint8_t* chunk, size_t len) {
    const uint8_t* frame = nullptr;
    size_t flen = 0;
    int rc = bl_reasm_push(&reasm_, chunk, len, &frame, &flen);
    if(rc == BL_OK && frame != nullptr) onFrame(frame, flen);
    /* reassembly errors silently drop; the far side retransmits/re-requests */
}

void BruceLinkBridge::onFrame(const uint8_t* frame, size_t len) {
    if(!established()) {
        uint8_t out[BL_MAX_MSG];
        size_t olen = 0;
        int rc = bl_session_handshake_rx(&session_, frame, len, out, sizeof(out), &olen);
        if(rc == BL_OK && olen) sendRaw(out, olen);
        return;
    }
    bl_frame_t f;
    if(bl_session_open(&session_, frame, len, &f) != BL_OK) return;
    handleRequest(&f);
}

void BruceLinkBridge::handleRequest(const bl_frame_t* f) {
    char buf[BL_MAX_PAYLOAD];
    switch(f->type) {
    case BL_MSG_PING:
        sendSealed(BL_MSG_PONG, BL_FLAG_RESPONSE, f->payload, f->payload_len);
        break;
    case BL_MSG_DEVICE_INFO_REQ: {
        size_t n = backend_->deviceInfo(buf, sizeof(buf));
        sendSealed(BL_MSG_DEVICE_INFO, BL_FLAG_RESPONSE, (uint8_t*)buf, (uint16_t)n);
        break;
    }
    case BL_MSG_CAPS_REQ: {
        uint8_t cb[BL_MAX_PAYLOAD];
        int n = bl_caps_serialize(&kRegistry, cb, sizeof(cb));
        if(n > 0) sendSealed(BL_MSG_CAPS, BL_FLAG_RESPONSE, cb, (uint16_t)n);
        else sendError(BL_ERR_INTERNAL);
        break;
    }
    case BL_MSG_STATUS_REQ: sendStatus(); break;
    case BL_MSG_CMD_REQ: {
        if(f->payload_len < 2) {
            sendError(BL_ERR_BAD_ARG);
            break;
        }
        uint16_t cap_id = (uint16_t)(f->payload[0] | (f->payload[1] << 8));
        const uint8_t* args = f->payload + 2;
        uint16_t arglen = (uint16_t)(f->payload_len - 2);

        if(cap_id == kCtrlEnableActive) {
            bool on = arglen >= 1 && args[0] != 0;
            int rc = bl_safety_enable_active(&safety_, on);
            if(rc == BL_OK) {
                int n = snprintf(buf, sizeof(buf), "active ops %s", on ? "enabled" : "disabled");
                sendSealed(BL_MSG_CMD_RESP, BL_FLAG_RESPONSE, (uint8_t*)buf, (uint16_t)n);
            } else {
                sendError(BL_ERR_ESTOPPED);
            }
            break;
        }

        const bl_capability_desc_t* d = bl_caps_find(&kRegistry, cap_id);
        if(d == nullptr) {
            sendError(BL_ERR_UNKNOWN_CAPABILITY);
            break;
        }
        if(d->op_class == BL_OP_PASSIVE) {
            if(bl_safety_authorize(&safety_, BL_OP_PASSIVE, nullptr, false, 0, backend_->nowMs()) !=
               BL_OK) {
                sendError(BL_ERR_ESTOPPED);
                break;
            }
            if(cap_id == BL_CAP_WIFI_SCAN) {
                size_t n = backend_->wifiScan(buf, sizeof(buf));
                sendSealed(BL_MSG_CMD_RESP, BL_FLAG_RESPONSE, (uint8_t*)buf, (uint16_t)n);
            } else {
                sendError(BL_ERR_BAD_ARG);
            }
        } else {
            /* active op: args = [channel(1)][bssid(6)] */
            if(arglen < 7) {
                sendError(BL_ERR_BAD_ARG);
                break;
            }
            uint8_t channel = args[0];
            const uint8_t* bssid = args + 1;
            bool need_target = (d->flags & BL_CAP_F_REQUIRES_TARGET) != 0;
            int rc = bl_safety_authorize(
                &safety_, BL_OP_ACTIVE, bssid, need_target, 1, backend_->nowMs());
            if(rc == BL_E_DENIED) {
                sendError(bl_safety_is_estopped(&safety_) ? BL_ERR_ESTOPPED : BL_ERR_NOT_AUTHORIZED);
                break;
            }
            if(rc == BL_E_LOCKED) {
                sendError(BL_ERR_RATE_LIMITED);
                break;
            }
            int sent = backend_->activeOp(cap_id, bssid, channel);
            if(sent < 0) {
                sendError(BL_ERR_INTERNAL);
            } else {
                int n = snprintf(buf, sizeof(buf), "op %04x: %d frames", cap_id, sent);
                sendSealed(BL_MSG_CMD_RESP, BL_FLAG_RESPONSE, (uint8_t*)buf, (uint16_t)n);
            }
        }
        break;
    }
    case BL_MSG_CAP_START: {
        /* args: [mode(1)][channel(1)] */
        if(f->payload_len < 2) {
            sendError(BL_ERR_BAD_ARG);
            break;
        }
        uint32_t sid = backend_->nowMs();
        int rc = bl_capture_start(
            &capture_, (bl_capture_mode_t)f->payload[0], f->payload[1], sid, sid);
        if(rc == BL_OK) sendStatus();
        else sendError(rc == BL_E_DENIED ? BL_ERR_NO_RESOURCE : BL_ERR_BAD_ARG);
        break;
    }
    case BL_MSG_CAP_STOP:
        bl_capture_stop(&capture_);
        sendStatus();
        break;
    case BL_MSG_CAP_STATUS: sendStatus(); break;
    case BL_MSG_ESTOP:
        bl_safety_estop(&safety_);
        if(bl_capture_is_running(&capture_)) bl_capture_stop(&capture_);
        sendSealed(BL_MSG_ESTOP, BL_FLAG_RESPONSE, nullptr, 0);
        break;
    default: sendError(BL_ERR_UNKNOWN_TYPE); break;
    }
}

void BruceLinkBridge::sendStatus() {
    if(bl_capture_is_running(&capture_)) bl_capture_poll(&capture_);
    /* status payload: text summary (keeps the Flipper simple) */
    char buf[BL_MAX_PAYLOAD];
    const char* st = bl_capture_is_running(&capture_) ? "running" : "idle";
    int n = snprintf(
        buf, sizeof(buf), "capture:%s pkts:%lu eapol:%lu hs:%lu estop:%d active:%d",
        st, (unsigned long)capture_.counts.packets, (unsigned long)capture_.counts.eapol,
        (unsigned long)capture_.counts.handshakes, bl_safety_is_estopped(&safety_) ? 1 : 0,
        bl_safety_active_ready(&safety_) ? 1 : 0);
    sendSealed(BL_MSG_STATUS, BL_FLAG_RESPONSE, (uint8_t*)buf, (uint16_t)n);
}

void BruceLinkBridge::sendSealed(uint8_t type, uint8_t flags, const uint8_t* pl, uint16_t len) {
    uint8_t out[BL_MAX_MSG];
    int n = bl_session_seal(&session_, type, flags, pl, len, out, sizeof(out));
    if(n > 0) sendRaw(out, (size_t)n);
}

void BruceLinkBridge::sendError(uint16_t err) {
    uint8_t pl[2] = {(uint8_t)(err & 0xFF), (uint8_t)(err >> 8)};
    sendSealed(BL_MSG_ERROR, BL_FLAG_RESPONSE | BL_FLAG_ERROR, pl, sizeof(pl));
}

void BruceLinkBridge::sendRaw(const uint8_t* frame, size_t len) {
    if(!tx_) return;
    unsigned i = 0;
    int done = 0;
    while(!done) {
        uint8_t chunk[BL_MAX_PAYLOAD + BL_FRAG_HDR];
        size_t clen = 0;
        int rc = bl_frag_chunk(frame, len, mtu_, i, chunk, sizeof(chunk), &clen);
        if(rc < 0) return;
        done = rc;
        tx_(tx_ctx_, chunk, clen);
        i++;
    }
}

void BruceLinkBridge::tick() {
    if(link_up_ && bl_capture_is_running(&capture_)) bl_capture_poll(&capture_);
}

} // namespace brucelink
