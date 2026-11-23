/*
 * BruceLink ESP32-S3 bridge — per-peer reconnect cooldown.
 *
 * The StickS3 (BLE central) connects to whatever advertises the public
 * BruceLink service UUID. When a peer fails to authenticate (bad proof or
 * handshake timeout) the transport drops it AND puts its address on a
 * short cooldown, so the next scan moves on instead of immediately
 * reconnecting to the same impostor and starving the paired Flipper.
 *
 * Portable (no NimBLE/Arduino deps), fixed storage, wrap-safe timing;
 * host-tested in tests/c/test_bridge.cpp.
 *
 * Residual risk: an attacker that rotates its address each attempt is not
 * stopped by a per-address table; it can still delay (not hijack) the
 * connection. Authentication itself is never weakened. See ADR 0001.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace brucelink {

class PeerCooldown {
public:
    static const size_t kSlots = 8;
    static const size_t kIdMax = 8; /* e.g. 6-byte BLE address + type */

    PeerCooldown();

    /* Block `id` until now_ms + duration_ms. Re-penalising extends the
     * block. When full, the entry expiring soonest is replaced. */
    void penalize(const uint8_t* id, size_t len, uint32_t now_ms, uint32_t duration_ms);

    /* True while `id` is blocked at now_ms. */
    bool blocked(const uint8_t* id, size_t len, uint32_t now_ms) const;

    void clear();

private:
    struct Entry {
        uint8_t id[kIdMax];
        uint8_t len;
        uint32_t until_ms;
        bool used;
    };
    static bool active(const Entry& e, uint32_t now_ms);
    static bool same(const Entry& e, const uint8_t* id, size_t len);
    Entry slots_[kSlots];
};

} // namespace brucelink
