/*
 * BruceLink ESP32-S3 bridge — per-peer reconnect cooldown.
 * SPDX-License-Identifier: MIT
 */
#include "BruceLinkPeerGuard.h"

#include <string.h>

namespace brucelink {

PeerCooldown::PeerCooldown() {
    clear();
}

void PeerCooldown::clear() {
    memset(slots_, 0, sizeof(slots_));
}

/* Wrap-safe "until is still in the future" across millis() rollover. */
bool PeerCooldown::active(const Entry& e, uint32_t now_ms) {
    return e.used && (int32_t)(e.until_ms - now_ms) > 0;
}

bool PeerCooldown::same(const Entry& e, const uint8_t* id, size_t len) {
    return e.used && e.len == len && memcmp(e.id, id, len) == 0;
}

void PeerCooldown::penalize(
    const uint8_t* id,
    size_t len,
    uint32_t now_ms,
    uint32_t duration_ms) {
    if(id == nullptr || len == 0) return;
    if(len > kIdMax) len = kIdMax;
    uint32_t until = now_ms + duration_ms;

    Entry* target = nullptr;
    for(size_t i = 0; i < kSlots && target == nullptr; i++)
        if(same(slots_[i], id, len)) target = &slots_[i]; /* existing entry */
    /* never shorten an active block */
    if(target != nullptr && active(*target, now_ms) && (int32_t)(target->until_ms - until) > 0)
        until = target->until_ms;
    for(size_t i = 0; i < kSlots && target == nullptr; i++)
        if(!active(slots_[i], now_ms)) target = &slots_[i]; /* free or expired */
    if(target == nullptr) { /* full: evict the entry expiring soonest */
        target = &slots_[0];
        for(size_t i = 1; i < kSlots; i++) {
            if((int32_t)(slots_[i].until_ms - target->until_ms) < 0) target = &slots_[i];
        }
    }
    memset(target->id, 0, sizeof(target->id));
    memcpy(target->id, id, len);
    target->len = (uint8_t)len;
    target->until_ms = until;
    target->used = true;
}

bool PeerCooldown::blocked(const uint8_t* id, size_t len, uint32_t now_ms) const {
    if(id == nullptr || len == 0) return false;
    if(len > kIdMax) len = kIdMax;
    for(size_t i = 0; i < kSlots; i++) {
        if(same(slots_[i], id, len) && active(slots_[i], now_ms)) return true;
    }
    return false;
}

} // namespace brucelink
