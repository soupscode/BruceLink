/*
 * BruceLink ESP32-S3 bridge core.
 * SPDX-License-Identifier: MIT
 */
#include "BruceLinkBridge.h"

extern "C" {
#include "bl_crypto.h"
#include "bl_demo_key.h"
#include "bl_textbuf.h"
}

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

static const uint8_t kChannelMin = 1;
static const uint8_t kChannelMax = 14;

static void bridge_rng(void* ctx, uint8_t* buf, size_t len) {
    BruceBackend* b = static_cast<BruceBackend*>(ctx);
    b->randomBytes(buf, len);
}

/* Length of the text actually present in buf: never more than the backend
 * reported, never past cap, and stopping at the terminator. Defends the
 * bridge against a backend that over-reports (e.g. summing snprintf). */
static uint16_t boundedTextLen(const char* buf, size_t reported, size_t cap) {
    size_t lim = reported < cap ? reported : cap;
    size_t n = 0;
    while(n < lim && buf[n] != '\0') n++;
    return (uint16_t)n;
}

BruceLinkBridge::BruceLinkBridge()
    : have_psk_(false), demo_key_(false), backend_(nullptr), tx_(nullptr), tx_ctx_(nullptr), mtu_(20),
      link_up_(false), link_up_ms_(0), pairing_(false), pair_link_(false), pair_foreign_(false),
      pair_taken_(false), penalize_(true), clean_drop_(false) {
    memset(psk_, 0, sizeof(psk_));
    memset(pair_name_, 0, sizeof(pair_name_));
    memset(&pair_, 0, sizeof(pair_));
    memset(&session_, 0, sizeof(session_));
    bl_safety_init(&safety_);
    bl_capture_init(&capture_, nullptr, nullptr); /* valid (IDLE) before begin() */
    bl_reasm_init(&reasm_);
}

BruceLinkBridge::~BruceLinkBridge() {
    bl_capture_stop(&capture_);
    bl_session_close(&session_);
    bl_pair_wipe(&pair_);
    bl_secure_zero(psk_, sizeof(psk_));
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
    setMtu(mtu);
    if(psk != nullptr) {
        setKey(psk);
    } else {
        bl_secure_zero(psk_, sizeof(psk_));
        have_psk_ = false;
        demo_key_ = false;
    }
    pairing_ = false;
    pair_link_ = false;
    bl_pair_wipe(&pair_);
    bl_safety_init(&safety_);
    /* Unconditionally valid; attach a backend only if the build has one. */
    bl_capture_init(&capture_, nullptr, nullptr);
    const bl_capture_backend_t* cbe = nullptr;
    void* cctx = nullptr;
    if(backend_ != nullptr && backend_->captureBackend(&cbe, &cctx) && cbe != nullptr) {
        bl_capture_init(&capture_, cbe, cctx);
    }
}

void BruceLinkBridge::setKey(const uint8_t psk[BL_KEY_LEN]) {
    /* The provisioned key lives in its own field: session_ is disposable
     * and bl_session_close() wipes its copy on every disconnect. */
    memcpy(psk_, psk, BL_KEY_LEN);
    have_psk_ = true;
    /* The demo key is published: a session made with it authenticates
     * nobody. Such a bridge serves passive requests for bring-up but never
     * transmits on a remote request (see handleCommand / handleActiveOp). */
    demo_key_ = memcmp(psk_, BRUCELINK_DEMO_PSK, BL_KEY_LEN) == 0;
}

void BruceLinkBridge::beginPairing(const char* local_name) {
    size_t n = 0;
    if(local_name != nullptr) {
        while(n < BL_PAIR_NAME_MAX && local_name[n] != '\0') n++;
        memcpy(pair_name_, local_name, n);
    }
    pair_name_[n] = '\0';
    pairing_ = true;
    pair_link_ = false;
    pair_foreign_ = false;
    bl_pair_wipe(&pair_);
}

void BruceLinkBridge::endPairing() {
    /* A link still up was opened for pairing; it carries no session.
     * Release it without holding it against the peer. */
    clean_drop_ = link_up_;
    pairing_ = false;
    pair_link_ = false;
    pair_foreign_ = false;
    bl_pair_wipe(&pair_);
}

void BruceLinkBridge::pairAnswer(bool accept) {
    if(!pairing_ || !pair_link_ || !link_up_) return;
    uint8_t out[BL_PAIR_FRAME_MAX];
    size_t olen = 0;
    if(bl_pair_accept(&pair_, accept, out, sizeof(out), &olen) == BL_OK && olen) sendRaw(out, olen);
}

bool BruceLinkBridge::pairTakeResult(uint8_t psk[BL_KEY_LEN], uint8_t key_id[BL_PAIR_KEY_ID_LEN]) {
    if(!pairing_ || !pair_link_ || pair_taken_) return false;
    if(bl_pair_result(&pair_, psk, key_id) != BL_OK) return false;
    bl_pair_wipe(&pair_);
    /* Keep reporting "done", but the key is gone: a second take must not
     * hand out the wiped (all-zero) buffer as a key. */
    pair_.state = BL_PS_DONE;
    pair_taken_ = true;
    return true;
}

/* Largest chunk sendRaw() can stage. */
static const size_t kMaxChunk = BL_MAX_PAYLOAD + BL_FRAG_HDR;

void BruceLinkBridge::setMtu(size_t mtu) {
    if(mtu < BL_MIN_CHUNK) mtu = BL_MIN_CHUNK;
    if(mtu > kMaxChunk) mtu = kMaxChunk;
    mtu_ = mtu;
}

void BruceLinkBridge::onConnect() {
    bl_reasm_init(&reasm_);
    bl_safety_reset(&safety_); /* new session: e-stop cleared, active ops off */
    link_up_ = true;
    link_up_ms_ = backend_ != nullptr ? backend_->nowMs() : 0;
    penalize_ = true;
    clean_drop_ = false;
    if(pairing_) {
        /* Every link is a fresh attempt; no session in pairing mode. */
        memset(&session_, 0, sizeof(session_));
        session_.state = BL_SS_CLOSED;
        pair_foreign_ = false;
        pair_taken_ = false;
        pair_link_ = backend_ != nullptr &&
                     bl_pair_init(
                         &pair_, BL_PAIR_RESPONDER, pair_name_, bridge_rng, backend_, link_up_ms_) ==
                         BL_OK;
        return;
    }
    if(!have_psk_ || backend_ == nullptr) {
        /* not provisioned: leave the session closed; handshakes are refused */
        memset(&session_, 0, sizeof(session_));
        session_.state = BL_SS_CLOSED;
        return;
    }
    bl_session_init(&session_, BL_ROLE_PERIPHERAL, psk_, bridge_rng, backend_);
}

void BruceLinkBridge::onDisconnect() {
    link_up_ = false;
    clean_drop_ = false;
    bl_safety_on_link_loss(&safety_); /* fail-safe: halt active ops */
    bl_capture_stop(&capture_); /* idempotent; also clears an errored capture */
    bl_session_close(&session_); /* wipes session secrets, not psk_ */
    bl_reasm_init(&reasm_);
    if(pairing_ && pair_link_) {
        /* Nobody left to tell; keep the outcome for the UI, drop secrets. */
        uint8_t out[BL_PAIR_FRAME_MAX];
        size_t olen = 0;
        (void)bl_pair_cancel(&pair_, out, sizeof(out), &olen);
    }
}

void BruceLinkBridge::onChunk(const uint8_t* chunk, size_t len) {
    const uint8_t* frame = nullptr;
    size_t flen = 0;
    int rc = bl_reasm_push(&reasm_, chunk, len, &frame, &flen);
    if(rc == BL_OK && frame != nullptr) onFrame(frame, flen);
    /* reassembly errors drop the partial frame; the peer re-requests */
}

void BruceLinkBridge::onPairFrame(const uint8_t* frame, size_t len) {
    if(!pair_link_) return;
    uint8_t out[BL_PAIR_FRAME_MAX];
    size_t olen = 0;
    int rc = bl_pair_rx(&pair_, frame, len, out, sizeof(out), &olen);
    if(olen) sendRaw(out, olen);
    if(rc == BL_E_STATE && bl_pair_state(&pair_) == BL_PS_WAIT_COMMIT) {
        /* Not a pairing Flipper (e.g. one that is starting a normal
         * session): let it go so a pairing one can be found. */
        pair_foreign_ = true;
    }
}

void BruceLinkBridge::onFrame(const uint8_t* frame, size_t len) {
    if(pairing_) {
        onPairFrame(frame, len);
        return;
    }
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

bool BruceLinkBridge::radioAllowed() {
    return bl_safety_authorize(&safety_, BL_OP_PASSIVE, nullptr, false, 0, backend_->nowMs()) ==
           BL_OK;
}

void BruceLinkBridge::handleRequest(const bl_frame_t* f) {
    switch(f->type) {
    case BL_MSG_PING:
        sendSealed(BL_MSG_PONG, BL_FLAG_RESPONSE, f->payload, f->payload_len);
        break;
    case BL_MSG_DEVICE_INFO_REQ: {
        char buf[BL_MAX_PAYLOAD];
        memset(buf, 0, sizeof(buf));
        size_t n = backend_->deviceInfo(buf, sizeof(buf));
        sendText(BL_MSG_DEVICE_INFO, buf, boundedTextLen(buf, n, sizeof(buf)));
        break;
    }
    case BL_MSG_CAPS_REQ: {
        uint8_t cb[BL_MAX_PAYLOAD];
        int n = bl_caps_serialize(&kRegistry, cb, sizeof(cb));
        if(n > 0)
            sendSealed(BL_MSG_CAPS, BL_FLAG_RESPONSE, cb, (uint16_t)n);
        else
            sendError(BL_ERR_INTERNAL);
        break;
    }
    case BL_MSG_STATUS_REQ:
    case BL_MSG_CAP_STATUS: sendStatus(); break;
    case BL_MSG_CMD_REQ: handleCommand(f); break;
    case BL_MSG_CAP_START: handleCaptureStart(f); break;
    case BL_MSG_CAP_STOP:
        bl_capture_stop(&capture_);
        sendStatus();
        break;
    case BL_MSG_ESTOP:
        /* Latches for the rest of this session; a new session resets it. */
        bl_safety_estop(&safety_);
        bl_capture_stop(&capture_);
        sendSealed(BL_MSG_ESTOP, BL_FLAG_RESPONSE, nullptr, 0);
        break;
    default: sendError(BL_ERR_UNKNOWN_TYPE); break;
    }
}

void BruceLinkBridge::handleCommand(const bl_frame_t* f) {
    if(f->payload_len < 2) {
        sendError(BL_ERR_BAD_ARG);
        return;
    }
    uint16_t cap_id = (uint16_t)(f->payload[0] | (f->payload[1] << 8));
    const uint8_t* args = f->payload + 2;
    uint16_t arglen = (uint16_t)(f->payload_len - 2);

    if(cap_id == kCtrlEnableActive) {
        bool on = arglen >= 1 && args[0] != 0;
        if(on && demo_key_) {
            sendError(BL_ERR_NOT_AUTHORIZED); /* no real key, no transmission */
            return;
        }
        if(bl_safety_enable_active(&safety_, on) != BL_OK) {
            sendError(BL_ERR_ESTOPPED);
            return;
        }
        char buf[32];
        bl_textbuf_t tb;
        bl_tb_init(&tb, buf, sizeof(buf));
        bl_tb_str(&tb, on ? "active ops enabled" : "active ops disabled");
        sendText(BL_MSG_CMD_RESP, buf, tb.len);
        return;
    }

    const bl_capability_desc_t* d = bl_caps_find(&kRegistry, cap_id);
    if(d == nullptr) {
        sendError(BL_ERR_UNKNOWN_CAPABILITY);
        return;
    }
    if(d->op_class == BL_OP_ACTIVE) {
        handleActiveOp(d, args, arglen);
        return;
    }
    /* passive radio use: still refused while e-stopped */
    if(!radioAllowed()) {
        sendError(BL_ERR_ESTOPPED);
        return;
    }
    if(cap_id == BL_CAP_WIFI_SCAN) {
        char buf[BL_MAX_PAYLOAD];
        memset(buf, 0, sizeof(buf));
        size_t n = backend_->wifiScan(buf, sizeof(buf));
        sendText(BL_MSG_CMD_RESP, buf, boundedTextLen(buf, n, sizeof(buf)));
    } else {
        sendError(BL_ERR_BAD_ARG);
    }
}

void BruceLinkBridge::handleActiveOp(
    const bl_capability_desc_t* d,
    const uint8_t* args,
    uint16_t arglen) {
    /* args = [channel(1)][bssid(6)] */
    if(arglen < 7) {
        sendError(BL_ERR_BAD_ARG);
        return;
    }
    const uint8_t channel = args[0];
    const uint8_t* bssid = args + 1;
    const bool need_target = (d->flags & BL_CAP_F_REQUIRES_TARGET) != 0;

    /* Distinguish refusal reasons for the operator; bl_safety_authorize
     * below re-checks all of them (defence in depth). */
    if(bl_safety_is_estopped(&safety_)) {
        sendError(BL_ERR_ESTOPPED);
        return;
    }
    if(demo_key_ || !safety_.active_enabled) {
        sendError(BL_ERR_NOT_AUTHORIZED);
        return;
    }
    if((need_target && !bl_safety_bssid_is_specific(bssid)) || channel < kChannelMin ||
       channel > kChannelMax) {
        sendError(BL_ERR_BAD_ARG);
        return;
    }

    /* Bound the op by what the rate window can still afford, reserve that
     * many frames, and hand the same number to the backend as a hard cap. */
    const uint32_t now = backend_->nowMs();
    uint32_t budget = bl_safety_budget(&safety_, now);
    if(budget > kMaxFramesPerActiveOp) budget = kMaxFramesPerActiveOp;
    if(budget == 0) {
        sendError(BL_ERR_RATE_LIMITED);
        return;
    }
    int rc = bl_safety_authorize(&safety_, BL_OP_ACTIVE, bssid, need_target, budget, now);
    if(rc == BL_E_LOCKED) {
        sendError(BL_ERR_RATE_LIMITED);
        return;
    }
    if(rc != BL_OK) {
        sendError(BL_ERR_TARGET_NOT_ALLOWED); /* only remaining reason: scope */
        return;
    }

    int sent = backend_->activeOp(d->id, bssid, channel, budget);
    if(sent < 0) {
        bl_safety_refund(&safety_, budget);
        sendError(BL_ERR_INTERNAL);
        return;
    }
    if((uint32_t)sent > budget) {
        /* Backend exceeded its authorized cap: fail safe. */
        bl_safety_estop(&safety_);
        bl_capture_stop(&capture_);
        sendError(BL_ERR_INTERNAL);
        return;
    }
    bl_safety_refund(&safety_, budget - (uint32_t)sent);

    char buf[48];
    bl_textbuf_t tb;
    bl_tb_init(&tb, buf, sizeof(buf));
    bl_tb_str(&tb, "op ");
    bl_tb_hex16(&tb, d->id);
    bl_tb_str(&tb, ": ");
    bl_tb_int(&tb, sent);
    bl_tb_str(&tb, " frames");
    sendText(BL_MSG_CMD_RESP, buf, tb.len);
}

void BruceLinkBridge::handleCaptureStart(const bl_frame_t* f) {
    /* args: [mode(1)][channel(1)] */
    if(f->payload_len < 2) {
        sendError(BL_ERR_BAD_ARG);
        return;
    }
    if(!radioAllowed()) { /* e-stop latched: no capture may (re)start */
        sendError(BL_ERR_ESTOPPED);
        return;
    }
    if(capture_.be == nullptr) {
        sendError(BL_ERR_CAPABILITY_DISABLED);
        return;
    }
    const uint8_t mode = f->payload[0];
    if(mode > (uint8_t)BL_CAP_MODE_DEAUTH_PCAP) { /* validate before enum cast */
        sendError(BL_ERR_BAD_ARG);
        return;
    }
    const uint32_t now = backend_->nowMs();
    int rc = bl_capture_start(&capture_, (bl_capture_mode_t)mode, f->payload[1], now, now);
    switch(rc) {
    case BL_OK: sendStatus(); break;
    case BL_E_BUSY: sendError(BL_ERR_BUSY); break;
    case BL_E_DENIED: sendError(BL_ERR_NO_RESOURCE); break; /* mode needs ext HW */
    case BL_E_ARG: sendError(BL_ERR_BAD_ARG); break;
    case BL_E_NO_RESOURCE: sendError(BL_ERR_NO_RESOURCE); break;
    default: sendError(BL_ERR_INTERNAL); break;
    }
}

void BruceLinkBridge::sendStatus() {
    /* A failing poll stops and flushes the backend inside bl_capture_poll. */
    if(bl_capture_is_running(&capture_)) (void)bl_capture_poll(&capture_);
    const char* st = "idle";
    if(capture_.state == BL_CAPST_RUNNING)
        st = "running";
    else if(capture_.state == BL_CAPST_ERROR)
        st = "error";

    char buf[BL_MAX_PAYLOAD];
    bl_textbuf_t tb;
    bl_tb_init(&tb, buf, sizeof(buf));
    bl_tb_str(&tb, "capture:");
    bl_tb_str(&tb, st);
    bl_tb_str(&tb, " pkts:");
    bl_tb_uint(&tb, capture_.counts.packets);
    bl_tb_str(&tb, " eapol:");
    bl_tb_uint(&tb, capture_.counts.eapol);
    bl_tb_str(&tb, " hs:");
    bl_tb_uint(&tb, capture_.counts.handshakes);
    bl_tb_str(&tb, " err:");
    bl_tb_int(&tb, capture_.last_error);
    bl_tb_str(&tb, " estop:");
    bl_tb_int(&tb, bl_safety_is_estopped(&safety_) ? 1 : 0);
    bl_tb_str(&tb, " active:");
    bl_tb_int(&tb, bl_safety_active_ready(&safety_) ? 1 : 0);
    bl_tb_str(&tb, " demo:");
    bl_tb_int(&tb, demo_key_ ? 1 : 0);
    sendText(BL_MSG_STATUS, buf, tb.len);
}

void BruceLinkBridge::sendText(uint8_t type, const char* text, size_t len) {
    if(len > BL_MAX_PAYLOAD) len = BL_MAX_PAYLOAD;
    sendSealed(
        type, BL_FLAG_RESPONSE, reinterpret_cast<const uint8_t*>(text), (uint16_t)len);
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
        uint8_t chunk[kMaxChunk];
        size_t clen = 0;
        int rc = bl_frag_chunk(frame, len, mtu_, i, chunk, sizeof(chunk), &clen);
        if(rc < 0) return;
        done = rc;
        tx_(tx_ctx_, chunk, clen);
        i++;
    }
}

bool BruceLinkBridge::penalizeOnDrop() const {
    return penalize_;
}

bool BruceLinkBridge::needsDisconnect() {
    if(!link_up_) return false;
    if(pairing_) {
        if(!pair_link_ || backend_ == nullptr) {
            penalize_ = false;
            return true;
        }
        const uint32_t now = backend_->nowMs();
        const bl_pair_state_t st = bl_pair_state(&pair_);
        if(pair_foreign_ ||
           (st == BL_PS_WAIT_COMMIT && (uint32_t)(now - link_up_ms_) >= kAuthTimeoutMs)) {
            /* Not pairing with us: move on, and do not come straight back. */
            penalize_ = true;
            return true;
        }
        /* An attempt that ended (accepted, rejected, timed out) frees the
         * link; the same Flipper may try again at once. */
        penalize_ = false;
        return st == BL_PS_DONE || st == BL_PS_FAILED;
    }
    if(clean_drop_) {
        /* the link that carried the pairing, after leaving pairing mode */
        penalize_ = false;
        return true;
    }
    penalize_ = true;
    if(established()) return false;
    /* terminal: proof failed, version mismatch, or never provisioned */
    if(session_.state == BL_SS_FAILED || session_.state == BL_SS_CLOSED) return true;
    if(backend_ == nullptr) return true;
    /* unsigned subtraction is wrap-safe across millis() rollover */
    return (uint32_t)(backend_->nowMs() - link_up_ms_) >= kAuthTimeoutMs;
}

void BruceLinkBridge::tick() {
    if(pairing_) {
        if(!link_up_ || !pair_link_ || backend_ == nullptr) return;
        uint8_t out[BL_PAIR_FRAME_MAX];
        size_t olen = 0;
        if(bl_pair_tick(&pair_, backend_->nowMs(), out, sizeof(out), &olen) && olen) {
            sendRaw(out, olen);
        }
        return;
    }
    /* A failing poll stops and flushes the backend (see bl_capture_poll). */
    if(link_up_ && bl_capture_is_running(&capture_)) (void)bl_capture_poll(&capture_);
}

} // namespace brucelink
