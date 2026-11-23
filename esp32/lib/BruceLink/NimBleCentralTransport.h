/*
 * BruceLink ESP32-S3 bridge — NimBLE central transport.
 *
 * The StickS3 is the BLE central: it scans for the Flipper's BruceLink
 * service, connects, discovers the RX (write) and TX (notify)
 * characteristics, forwards inbound notifications to the bridge as
 * chunks, and provides the bridge's tx path by writing to RX.
 *
 * Threading: NimBLE callbacks run on the NimBLE host task. They NEVER
 * touch the bridge; they only record events (chunks into a bounded queue;
 * disconnect / MTU / scan target latched under a spinlock). loop(), on the
 * application task, is the single owner of the bridge.
 *
 * Link supervision: transport connectivity never substitutes for an
 * authenticated session. A peer that fails the handshake, or does not
 * complete it within kAuthTimeoutMs, is disconnected and its address is
 * put on a cooldown so the next scan moves on (see BruceLinkPeerGuard).
 *
 * UUIDs here must match the Flipper peripheral profile
 * (docs/decisions/0001-ble-transport.md).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "BruceLinkBridge.h"
#include "BruceLinkPeerGuard.h"

extern "C" {
#include "bl_uuid.h"
}

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace brucelink {

// 128-bit custom profile UUIDs: defined once in the shared core (bl_uuid.h)
// together with the Flipper's byte form, and tested to agree.
#define BRUCELINK_SVC_UUID BL_UUID_SERVICE_STR
#define BRUCELINK_RX_UUID BL_UUID_RX_STR // central -> peripheral (write)
#define BRUCELINK_TX_UUID BL_UUID_TX_STR // peripheral -> central (notify)

class NimBleCentralTransport : public NimBLEScanCallbacks, public NimBLEClientCallbacks {
public:
    /* How long a peer that failed authentication is skipped. */
    static const uint32_t kPeerCooldownMs = 30000;
    /* Inbound chunk queue (NimBLE task -> loop). */
    static const size_t kEventQueueDepth = 16;
    static const size_t kMaxChunk = BL_BLE_CHUNK_MAX; /* largest Flipper update */
    /* Upper bound on one blocking connection attempt. */
    static const uint32_t kConnectTimeoutMs = 5000;

    NimBleCentralTransport();
    // Initialise NimBLE and the bridge; psk is the provisioned pairing key
    // (nullptr when there is none yet: put the bridge in pairing mode before
    // the first loop(), or it drops every link). Clears the peer cooldowns.
    void begin(BruceLinkBridge* bridge, const uint8_t psk[BL_KEY_LEN], BruceBackend* backend);
    // Start scanning for a BruceLink peripheral.
    void startScan(uint32_t duration_s = 0);
    // Single owner of the bridge: process events, supervise, connect, poll.
    void loop();
    // Shut down: stop scanning, drop the peer (the bridge sees a link loss,
    // so captures stop and active ops halt) and release NimBLE. Call from
    // the task that runs loop(), or after that task has stopped.
    void end();
    bool connected() const {
        return connected_;
    }

    // NimBLEScanCallbacks (NimBLE task)
    void onResult(const NimBLEAdvertisedDevice* dev) override;
    void onScanEnd(const NimBLEScanResults& results, int reason) override;
    // NimBLEClientCallbacks (NimBLE task)
    void onDisconnect(NimBLEClient* client, int reason) override;
    void onMTUChange(NimBLEClient* client, uint16_t mtu) override;

private:
    struct ChunkEvent {
        uint16_t len;
        uint8_t data[kMaxChunk];
    };

    static void txTrampoline(void* ctx, const uint8_t* chunk, size_t len);
    void onNotify(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify);
    void drainChunks();
    void handleLinkLoss();
    void connectPending();
    /* Disconnect the current peer; with penalize, also put it on cooldown
     * (protocol mismatch / failed or slow authentication). */
    void dropPeer(bool penalize);
    static size_t addrId(const NimBLEAddress& a, uint8_t out[PeerCooldown::kIdMax]);
    static size_t usableMtu(uint16_t att_mtu);

    BruceLinkBridge* bridge_;
    BruceBackend* backend_;
    NimBLEClient* client_;
    NimBLERemoteCharacteristic* rx_; // we write here
    NimBLERemoteCharacteristic* tx_; // we subscribe here
    QueueHandle_t chunks_;
    bool chunks_with_caps_ = false; /* created with xQueueCreateWithCaps */

    /* --- shared with NimBLE callbacks: guarded by lock_ --- */
    portMUX_TYPE lock_;
    PeerCooldown cooldown_;
    NimBLEAddress pending_addr_;
    bool have_pending_;
    bool link_lost_;
    uint16_t pending_mtu_;

    /* --- loop()-only state --- */
    uint8_t peer_id_[PeerCooldown::kIdMax];
    size_t peer_id_len_;
    bool connected_; // link up AND bridge_->onConnect() done
    bool dropping_; // we asked NimBLE to disconnect; waiting for it
};

} // namespace brucelink
