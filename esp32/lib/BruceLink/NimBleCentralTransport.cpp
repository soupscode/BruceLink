/*
 * BruceLink ESP32-S3 bridge — NimBLE central transport (NimBLE-Arduino 2.5.x).
 * SPDX-License-Identifier: MIT
 */
#include "NimBleCentralTransport.h"

#include <functional>

namespace brucelink {

NimBleCentralTransport::NimBleCentralTransport()
    : bridge_(nullptr), backend_(nullptr), client_(nullptr), rx_(nullptr), tx_(nullptr),
      have_target_(false), connected_(false), mtu_(20) {}

void NimBleCentralTransport::begin(
    BruceLinkBridge* bridge, const uint8_t psk[BL_KEY_LEN], BruceBackend* backend) {
    bridge_ = bridge;
    backend_ = backend;
    NimBLEDevice::init("BruceLink");
    NimBLEDevice::setMTU(247); // request a larger ATT MTU for throughput
    bridge_->begin(psk, backend_, &NimBleCentralTransport::txTrampoline, this, mtu_);
}

void NimBleCentralTransport::startScan(uint32_t duration_s) {
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(this, false);
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(80);
    scan->start(duration_s * 1000, false, true);
}

void NimBleCentralTransport::onResult(const NimBLEAdvertisedDevice* dev) {
    if(dev->isAdvertisingService(NimBLEUUID(BRUCELINK_SVC_UUID))) {
        found_ = *dev;
        have_target_ = true;
        NimBLEDevice::getScan()->stop();
    }
}

void NimBleCentralTransport::onScanEnd(const NimBLEScanResults& results, int reason) {
    (void)results;
    (void)reason;
    // if nothing found, loop() will restart scanning
}

bool NimBleCentralTransport::discover(NimBLEClient* client) {
    NimBLERemoteService* svc = client->getService(NimBLEUUID(BRUCELINK_SVC_UUID));
    if(svc == nullptr) return false;
    rx_ = svc->getCharacteristic(NimBLEUUID(BRUCELINK_RX_UUID));
    tx_ = svc->getCharacteristic(NimBLEUUID(BRUCELINK_TX_UUID));
    if(rx_ == nullptr || tx_ == nullptr) return false;
    if(!tx_->canNotify()) return false;
    using namespace std::placeholders;
    NimBLERemoteCharacteristic::notify_callback cb =
        std::bind(&NimBleCentralTransport::onNotify, this, _1, _2, _3, _4);
    if(!tx_->subscribe(true, cb)) return false;
    return true;
}

void NimBleCentralTransport::onConnect(NimBLEClient* client) {
    // ATT header is 3 bytes; usable notify/write payload = MTU - 3.
    mtu_ = client->getMTU() > BL_FRAG_HDR + 4 ? (size_t)client->getMTU() - 3 : 20;
    if(bridge_) bridge_->setMtu(mtu_); // bridge fragments responses at this size
}

void NimBleCentralTransport::onDisconnect(NimBLEClient* client, int reason) {
    (void)client;
    (void)reason;
    connected_ = false;
    rx_ = nullptr;
    tx_ = nullptr;
    if(bridge_) bridge_->onDisconnect();
    have_target_ = false;
}

void NimBleCentralTransport::onNotify(
    NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
    (void)ch;
    (void)isNotify;
    if(bridge_) bridge_->onChunk(data, len);
}

void NimBleCentralTransport::txTrampoline(void* ctx, const uint8_t* chunk, size_t len) {
    NimBleCentralTransport* self = static_cast<NimBleCentralTransport*>(ctx);
    if(self->rx_ && self->connected_) {
        self->rx_->writeValue(chunk, len, false); // write without response
    }
}

void NimBleCentralTransport::loop() {
    if(!connected_ && have_target_) {
        if(client_ == nullptr) client_ = NimBLEDevice::createClient();
        client_->setClientCallbacks(this, false);
        if(client_->connect(&found_)) {
            if(discover(client_)) {
                connected_ = true;
                bridge_->onConnect();
            } else {
                client_->disconnect();
            }
        }
        have_target_ = false;
    }
    if(!connected_ && !have_target_ && !NimBLEDevice::getScan()->isScanning()) {
        startScan(0);
    }
    if(bridge_) bridge_->tick();
}

} // namespace brucelink
