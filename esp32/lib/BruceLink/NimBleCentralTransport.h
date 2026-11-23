/*
 * BruceLink ESP32-S3 bridge — NimBLE central transport.
 *
 * The StickS3 is the BLE central: it scans for the Flipper's BruceLink
 * service, connects, discovers the RX (write) and TX (notify)
 * characteristics, forwards inbound notifications to the bridge as
 * chunks, and provides the bridge's tx path by writing to RX.
 *
 * UUIDs here must match the Flipper peripheral profile
 * (docs/decisions/0001-ble-transport.md).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "BruceLinkBridge.h"

#include <NimBLEDevice.h>

namespace brucelink {

// 128-bit custom profile UUIDs (shared with the Flipper FAP profile).
#define BRUCELINK_SVC_UUID "a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00"
#define BRUCELINK_RX_UUID "a1b20002-7a9c-4b1d-9c2e-5f6a7b8c9d00" // central -> peripheral (write)
#define BRUCELINK_TX_UUID "a1b20003-7a9c-4b1d-9c2e-5f6a7b8c9d00" // peripheral -> central (notify)

class NimBleCentralTransport : public NimBLEScanCallbacks, public NimBLEClientCallbacks {
public:
    NimBleCentralTransport();
    // Initialise NimBLE and the bridge; psk is the provisioned pairing key.
    void begin(BruceLinkBridge* bridge, const uint8_t psk[BL_KEY_LEN], BruceBackend* backend);
    // Start scanning for a BruceLink peripheral.
    void startScan(uint32_t duration_s = 0);
    // Pump periodic work (capture polling, reconnect). Call from loop().
    void loop();
    bool connected() const {
        return connected_;
    }

    // NimBLEScanCallbacks
    void onResult(const NimBLEAdvertisedDevice* dev) override;
    void onScanEnd(const NimBLEScanResults& results, int reason) override;
    // NimBLEClientCallbacks
    void onConnect(NimBLEClient* client) override;
    void onDisconnect(NimBLEClient* client, int reason) override;

private:
    static void txTrampoline(void* ctx, const uint8_t* chunk, size_t len);
    void onNotify(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify);
    bool discover(NimBLEClient* client);

    BruceLinkBridge* bridge_;
    BruceBackend* backend_;
    NimBLEClient* client_;
    NimBLERemoteCharacteristic* rx_; // we write here
    NimBLERemoteCharacteristic* tx_; // we subscribe here
    NimBLEAdvertisedDevice found_;
    bool have_target_;
    bool connected_;
    size_t mtu_;
};

} // namespace brucelink
