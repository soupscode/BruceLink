/*
 * BruceLink ↔ Bruce glue (Bruce 1.17). Builds only inside the Bruce tree.
 * Status: written against audited Bruce symbols; NOT yet compiled in the
 * Bruce tree or run on hardware. See LOCAL_HANDOFF.md.
 * SPDX-License-Identifier: MIT
 */
#include "BruceBackendImpl.h"

#include "bruce_brucelink_hooks.h" // thin headless hooks added by the BruceLink patch

#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#include <globals.h> // Bruce: wifiConnected, wifiIP, sdcardMounted, ...

extern int getBattery(); // boards/m5stack-sticks3/interface.cpp:114
extern bool isCharging(); // boards/m5stack-sticks3/interface.cpp:139

namespace brucelink {

size_t BruceBackendImpl::deviceInfo(char* out, size_t cap) {
    // Mirrors Bruce's infoCallback (util_commands.cpp:99) fields.
    int n = snprintf(
        out, cap,
        "Bruce %s (%s)\n%s\nBatt %d%%%s\nHeap %u  PSRAM %u\nWi-Fi %s %s",
        BRUCE_VERSION, GIT_COMMIT_HASH, DEVICE_NAME, getBattery(),
        isCharging() ? " (chg)" : "", (unsigned)ESP.getFreeHeap(),
        (unsigned)ESP.getFreePsram(), wifiConnected ? "up" : "down",
        wifiConnected ? wifiIP.c_str() : "");
    return n < 0 ? 0 : ((size_t)n < cap ? (size_t)n : cap - 1);
}

size_t BruceBackendImpl::wifiScan(char* out, size_t cap) {
    // Same calls as Bruce's native_wifiScan (bjs_interpreter/wifi_js.cpp:76).
    int found = WiFi.scanNetworks(false, true);
    size_t w = 0;
    int n = snprintf(out, cap, "%d networks\n", found < 0 ? 0 : found);
    if(n > 0) w = (size_t)n;
    for(int i = 0; i < found && w < cap; i++) {
        n = snprintf(
            out + w, cap - w, "%.24s %ddBm ch%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
            WiFi.channel(i));
        if(n <= 0) break;
        w += (size_t)n;
    }
    WiFi.scanDelete();
    return w < cap ? w : cap - 1;
}

// --- capture backend: drives Bruce's existing sniffer via headless hooks ---

static int cap_start(void*, bl_capture_mode_t mode, uint8_t channel) {
    return bruce_brucelink_sniffer_start((int)mode, channel) ? BL_OK : BL_E_NO_RESOURCE;
}
static int cap_set_channel(void*, uint8_t channel) {
    return bruce_brucelink_sniffer_set_channel(channel) ? BL_OK : BL_E_ARG;
}
static int cap_poll(void*, bl_capture_counts_t* c) {
    BruceSnifferStatus s;
    if(!bruce_brucelink_sniffer_status(&s)) return BL_E_STATE;
    c->packets = s.packets;
    c->eapol = s.eapol;
    c->handshakes = s.handshakes;
    c->beacons = s.beacons;
    c->deauth = s.deauth;
    c->free_bytes = s.free_bytes;
    // Bruce stops promiscuous when LittleFS fills (sniffer.cpp:950); surface it.
    return s.storage_full ? BL_E_NO_RESOURCE : BL_OK;
}
static int cap_stop(void*) {
    bruce_brucelink_sniffer_stop();
    return BL_OK;
}
static int cap_mode_available(void*, bl_capture_mode_t mode) {
    // Full raw pcap is SD-gated upstream (sniffer_full_mode_available).
    if(mode == BL_CAP_MODE_RAW) return bruce_brucelink_raw_mode_available() ? 1 : 0;
    return 1;
}
static const bl_capture_backend_t kCaptureBackend = {
    cap_start, cap_set_channel, cap_poll, cap_stop, cap_mode_available};

bool BruceBackendImpl::captureBackend(const bl_capture_backend_t** be, void** ctx) {
    *be = &kCaptureBackend;
    *ctx = nullptr;
    return true;
}

int BruceBackendImpl::activeOp(uint16_t cap_id, const uint8_t* target, uint8_t channel) {
    // The bridge has already passed the remote safety gate. Forward to
    // Bruce's EXISTING transmit primitive; BruceLink builds no frames.
    switch(cap_id) {
    case BL_CAP_DEAUTH: return bruce_brucelink_deauth(target, channel);
    default: return -1; // not exposed
    }
}

uint32_t BruceBackendImpl::nowMs() {
    return millis();
}

void BruceBackendImpl::randomBytes(uint8_t* out, size_t len) {
    esp_fill_random(out, len);
}

} // namespace brucelink
