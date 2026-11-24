/*
 * BruceLink ↔ Bruce glue (Bruce 1.17). Builds only inside the Bruce tree.
 * Status: see docs/STATUS.md (compile status in the Bruce tree, hardware).
 * SPDX-License-Identifier: MIT
 */
#include "BruceBackendImpl.h"

#include <BruceLinkFormat.h>
#include "BruceLinkJobs.h"
#include "bruce_brucelink_hooks.h" // thin headless hooks added by the BruceLink patch

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <core/radio_mem.h> // Bruce: radioHasMemForWifi
#include <globals.h> // Bruce: wifiConnected, wifiIP, sdcardMounted, ...

extern int getBattery(); // boards/m5stack-sticks3/interface.cpp:114
extern bool isCharging(); // boards/m5stack-sticks3/interface.cpp:139

namespace brucelink {


size_t BruceBackendImpl::deviceInfo(char* out, size_t cap) {
    // Mirrors Bruce's infoCallback (util_commands.cpp:99) fields. Built with
    // the bounded text builder: the returned length is always the text
    // actually written (never snprintf's would-have-written length).
    bl_textbuf_t tb;
    bl_tb_init(&tb, out, cap);
    bl_tb_str(&tb, "Bruce ");
    bl_tb_str(&tb, BRUCE_VERSION);
    bl_tb_str(&tb, " (");
    bl_tb_str(&tb, GIT_COMMIT_HASH);
    bl_tb_str(&tb, ")\n");
    bl_tb_str(&tb, DEVICE_NAME);
    bl_tb_str(&tb, "\nBatt ");
    bl_tb_int(&tb, getBattery());
    bl_tb_str(&tb, isCharging() ? "% (chg)" : "%");
    bl_tb_str(&tb, "\nHeap "); // internal RAM: what the radios compete for
    bl_tb_uint(&tb, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bl_tb_str(&tb, "  PSRAM ");
    bl_tb_uint(&tb, (unsigned long)ESP.getFreePsram());
    bl_tb_str(&tb, "\nDMA blk "); // what gates starting a radio (radio_mem.h)
    bl_tb_uint(&tb, (unsigned long)radioLargestDmaBlock());
    bl_tb_str(&tb, "\nWi-Fi ");
    bl_tb_str(&tb, wifiConnected ? "up " : "down");
    if(wifiConnected) bl_tb_sanitized(&tb, wifiIP.c_str(), 40);
    return tb.len;
}

/* One line on the USB serial log per step, with the memory figures that
 * decide whether a radio can start (see Bruce's core/radio_mem.h). Used to
 * find where a remote operation stops when it does not come back. */
void logStep(const char* what) {
    Serial.printf(
        "[BruceLink] %s | internal %lu dma-blk %lu psram %lu stack-left %lu | %lu ms\n", what,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)radioLargestDmaBlock(), (unsigned long)ESP.getFreePsram(),
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr), (unsigned long)millis());
    Serial.flush();
}

bl_job_record_t* BruceBackendImpl::jobRecord() {
    return jobRecordStorage(); // RTC memory: survives the restart (BruceLinkJobs)
}

void BruceBackendImpl::jobRestart() {
    jobRestartRequest(); // the bridge task restarts once the reply has left
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

int BruceBackendImpl::activeOp(
    uint16_t cap_id,
    const uint8_t* target,
    uint8_t channel,
    uint32_t max_frames) {
    // The bridge has already passed the remote safety gate and reserved
    // max_frames from the rate window. Forward to Bruce's EXISTING transmit
    // primitive with that hard cap; BruceLink builds no frames.
    switch(cap_id) {
    case BL_CAP_DEAUTH: return bruce_brucelink_deauth(target, channel, max_frames);
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
