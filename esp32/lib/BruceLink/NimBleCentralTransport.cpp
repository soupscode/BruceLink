/*
 * BruceLink ESP32-S3 bridge — NimBLE central transport (NimBLE-Arduino 2.5.x).
 * SPDX-License-Identifier: MIT
 */
#include "NimBleCentralTransport.h"

#include <esp32-hal-log.h> // log_e
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h> // xQueueCreateWithCaps

#include <functional>
#include <string.h>

namespace brucelink {

NimBleCentralTransport::NimBleCentralTransport()
    : bridge_(nullptr), backend_(nullptr), client_(nullptr), rx_(nullptr), tx_(nullptr),
      chunks_(nullptr), have_pending_(false), link_lost_(false), pending_mtu_(0),
      peer_id_len_(0), connected_(false), dropping_(false) {
    portMUX_INITIALIZE(&lock_);
    memset(peer_id_, 0, sizeof(peer_id_));
}

void NimBleCentralTransport::begin(
    BruceLinkBridge* bridge, const uint8_t psk[BL_KEY_LEN], BruceBackend* backend) {
    bridge_ = bridge;
    backend_ = backend;
    cooldown_.clear(); // a fresh start (or a switch to pairing) owes nobody a penalty
    // ~4 kB of chunk storage: keep it out of internal RAM when there is
    // PSRAM (only tasks touch this queue, never an ISR).
    chunks_ = xQueueCreateWithCaps(kEventQueueDepth, sizeof(ChunkEvent), MALLOC_CAP_SPIRAM);
    chunks_with_caps_ = chunks_ != nullptr;
    if(chunks_ == nullptr) chunks_ = xQueueCreate(kEventQueueDepth, sizeof(ChunkEvent));
    NimBLEDevice::init("BruceLink");
    NimBLEDevice::setMTU(247); // request a larger ATT MTU for throughput
    bridge_->begin(psk, backend_, &NimBleCentralTransport::txTrampoline, this, usableMtu(23));
}

size_t NimBleCentralTransport::usableMtu(uint16_t att_mtu) {
    // Shared rule (bl_uuid.h): MTU - 3, never above what the Flipper's
    // characteristics accept, whatever MTU NimBLE ends up negotiating.
    return bl_ble_chunk_for_mtu(att_mtu);
}

size_t NimBleCentralTransport::addrId(const NimBLEAddress& a, uint8_t out[PeerCooldown::kIdMax]) {
    memcpy(out, a.getVal(), 6);
    out[6] = a.getType(); // public vs random are different identities
    return 7;
}

void NimBleCentralTransport::startScan(uint32_t duration_s) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(this, false);
    // Callback only: by default NimBLE keeps every advertiser it has ever
    // seen (no upper bound), in internal RAM, for as long as the scan
    // object lives. This scan runs whenever no Flipper is linked, and
    // internal RAM is what Wi-Fi needs next to BLE (hardware, 2026-11-23).
    scan->setMaxResults(0);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->start(duration_s * 1000, false, true);
}

/* ---------------------------------------------------- NimBLE task */

void NimBleCentralTransport::onResult(const NimBLEAdvertisedDevice* dev) {
    if(!dev->isAdvertisingService(NimBLEUUID(BRUCELINK_SVC_UUID))) return;
    uint8_t id[PeerCooldown::kIdMax];
    size_t n = addrId(dev->getAddress(), id);
    const uint32_t now = backend_->nowMs();
    bool take = false;
    portENTER_CRITICAL(&lock_);
    if(!have_pending_ && !cooldown_.blocked(id, n, now)) {
        pending_addr_ = dev->getAddress(); // POD copy, no allocation
        have_pending_ = true;
        take = true;
    }
    portEXIT_CRITICAL(&lock_);
    if(take) NimBLEDevice::getScan()->stop(); // blocked peers: keep scanning
}

void NimBleCentralTransport::onScanEnd(const NimBLEScanResults& results, int reason) {
    (void)results;
    (void)reason;
    // loop() restarts scanning when idle
}

void NimBleCentralTransport::onDisconnect(NimBLEClient* client, int reason) {
    (void)client;
    (void)reason;
    portENTER_CRITICAL(&lock_);
    link_lost_ = true; // latched, never dropped (unlike a full queue)
    portEXIT_CRITICAL(&lock_);
}

void NimBleCentralTransport::onMTUChange(NimBLEClient* client, uint16_t mtu) {
    (void)client;
    portENTER_CRITICAL(&lock_);
    pending_mtu_ = mtu;
    portEXIT_CRITICAL(&lock_);
}

void NimBleCentralTransport::onNotify(
    NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
    (void)ch;
    (void)isNotify;
    if(chunks_ == nullptr || len == 0 || len > kMaxChunk) return; // oversize: not ours
    ChunkEvent ev;
    ev.len = (uint16_t)len;
    memcpy(ev.data, data, len);
    // Never block the NimBLE host task; a dropped chunk fails reassembly
    // and the Flipper re-requests.
    (void)xQueueSend(chunks_, &ev, 0);
}

/* ------------------------------------------------ application task */

void NimBleCentralTransport::txTrampoline(void* ctx, const uint8_t* chunk, size_t len) {
    NimBleCentralTransport* self = static_cast<NimBleCentralTransport*>(ctx);
    if(self->rx_ && self->connected_) {
        // Write without response. A failure here (the BLE stack out of
        // memory, for one) means the Flipper never sees this frame and its
        // request times out; make that visible on the serial log.
        if(!self->rx_->writeValue(chunk, len, false)) {
            log_e("BruceLink: GATT write of %u bytes failed", (unsigned)len);
        }
    }
}

void NimBleCentralTransport::drainChunks() {
    ChunkEvent ev;
    while(chunks_ != nullptr && xQueueReceive(chunks_, &ev, 0) == pdTRUE) {
        if(connected_) bridge_->onChunk(ev.data, ev.len); // else: stale, drop
    }
}

void NimBleCentralTransport::handleLinkLoss() {
    if(connected_) bridge_->onDisconnect(); // fail-safe: e-stop + stop capture
    connected_ = false;
    dropping_ = false;
    rx_ = nullptr;
    tx_ = nullptr;
    peer_id_len_ = 0;
}

void NimBleCentralTransport::dropPeer(bool penalize) {
    if(dropping_) return;
    if(penalize && peer_id_len_ > 0) {
        const uint32_t now = backend_->nowMs();
        portENTER_CRITICAL(&lock_);
        cooldown_.penalize(peer_id_, peer_id_len_, now, kPeerCooldownMs);
        portEXIT_CRITICAL(&lock_);
    }
    dropping_ = true;
    if(client_ == nullptr || !client_->disconnect()) {
        handleLinkLoss(); // nothing to wait for
    }
}

void NimBleCentralTransport::connectPending() {
    NimBLEAddress addr;
    bool have = false;
    portENTER_CRITICAL(&lock_);
    if(have_pending_) {
        addr = pending_addr_;
        have_pending_ = false;
        have = true;
    }
    portEXIT_CRITICAL(&lock_);
    if(!have) return;

    if(client_ == nullptr) client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(this, false);
    // connect() blocks the loop; NimBLE's default is 30 s. A Flipper that
    // has stopped advertising should not stall supervision or shutdown.
    client_->setConnectTimeout(kConnectTimeoutMs);
    if(!client_->connect(addr)) return; // radio-level failure: just rescan

    peer_id_len_ = addrId(addr, peer_id_);
    NimBLERemoteService* svc = client_->getService(NimBLEUUID(BRUCELINK_SVC_UUID));
    rx_ = svc ? svc->getCharacteristic(NimBLEUUID(BRUCELINK_RX_UUID)) : nullptr;
    tx_ = svc ? svc->getCharacteristic(NimBLEUUID(BRUCELINK_TX_UUID)) : nullptr;
    if(rx_ == nullptr || tx_ == nullptr || !tx_->canNotify()) {
        dropPeer(true); // advertises our UUID but is not a BruceLink peripheral
        return;
    }

    // Bring the bridge up BEFORE subscribing, so a HELLO notified as soon
    // as the subscription is live is queued for an already-reset session.
    connected_ = true;
    bridge_->onConnect();
    bridge_->setMtu(usableMtu(client_->getMTU()));

    using namespace std::placeholders;
    NimBLERemoteCharacteristic::notify_callback cb =
        std::bind(&NimBleCentralTransport::onNotify, this, _1, _2, _3, _4);
    if(!tx_->subscribe(true, cb)) dropPeer(false); // likely radio trouble: no penalty
}

void NimBleCentralTransport::end() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if(scan != nullptr) {
        scan->stop();
        scan->setScanCallbacks(nullptr, false);
    }
    if(connected_ && bridge_ != nullptr) bridge_->onDisconnect(); // fail-safe first
    connected_ = false;
    dropping_ = false;
    rx_ = nullptr;
    tx_ = nullptr;
    peer_id_len_ = 0;
    client_ = nullptr; // owned by NimBLE; deinit(true) below deletes it
    NimBLEDevice::deinit(true);
    portENTER_CRITICAL(&lock_);
    have_pending_ = false;
    link_lost_ = false;
    pending_mtu_ = 0;
    portEXIT_CRITICAL(&lock_);
    if(chunks_ != nullptr) {
        if(chunks_with_caps_) vQueueDeleteWithCaps(chunks_);
        else vQueueDelete(chunks_);
        chunks_ = nullptr;
    }
}

void NimBleCentralTransport::loop() {
    // 1. inbound data for the current link
    drainChunks();

    // 2. latched NimBLE events
    bool lost = false;
    uint16_t mtu = 0;
    portENTER_CRITICAL(&lock_);
    lost = link_lost_;
    link_lost_ = false;
    mtu = pending_mtu_;
    pending_mtu_ = 0;
    portEXIT_CRITICAL(&lock_);
    if(lost) handleLinkLoss();
    if(mtu != 0 && connected_) bridge_->setMtu(usableMtu(mtu));

    // 3. supervision: no authenticated session in time -> drop (+ cooldown,
    //    unless the peer was genuinely pairing; see penalizeOnDrop)
    if(connected_ && !dropping_ && bridge_->needsDisconnect()) dropPeer(bridge_->penalizeOnDrop());

    // 4. connect / rescan when idle
    const bool idle = !connected_ && !dropping_ && (client_ == nullptr || !client_->isConnected());
    if(idle) connectPending();
    if(!connected_ && !dropping_ && !NimBLEDevice::getScan()->isScanning()) {
        bool pending = false;
        portENTER_CRITICAL(&lock_);
        pending = have_pending_;
        portEXIT_CRITICAL(&lock_);
        if(!pending) startScan(0);
    }

    if(bridge_) bridge_->tick();
}

} // namespace brucelink
