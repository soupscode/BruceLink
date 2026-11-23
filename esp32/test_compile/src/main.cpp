/*
 * BruceLink ESP32-S3 bridge — standalone compile/link check.
 *
 * This is NOT the Bruce firmware. It compiles the transport-independent
 * bridge core + the NimBLE central transport + the shared protocol core
 * against NimBLE-Arduino 2.5.1 for the ESP32-S3, with a stub backend in
 * place of the real Bruce glue (which lives in src/modules/brucelink on
 * the Bruce tree and needs the full Bruce build). It proves the bridge
 * and NimBLE usage build for the target; on-device behaviour is verified
 * in the hardware session.
 * SPDX-License-Identifier: MIT
 */
#include "bridge/NimBleCentralTransport.h"

#include <Arduino.h>
#include <esp_random.h>

using namespace brucelink;

// --- stub capture backend (no radio) --------------------------------------
static int stub_cap_start(void*, bl_capture_mode_t, uint8_t) {
    return BL_OK;
}
static int stub_cap_setch(void*, uint8_t) {
    return BL_OK;
}
static int stub_cap_poll(void*, bl_capture_counts_t*) {
    return BL_OK;
}
static int stub_cap_stop(void*) {
    return BL_OK;
}
static int stub_cap_avail(void*, bl_capture_mode_t m) {
    return m == BL_CAP_MODE_RAW ? 0 : 1; // no SD -> raw unavailable
}
static const bl_capture_backend_t STUB_CAP = {
    stub_cap_start, stub_cap_setch, stub_cap_poll, stub_cap_stop, stub_cap_avail};

// --- stub Bruce backend ---------------------------------------------------
class StubBackend : public BruceBackend {
public:
    size_t deviceInfo(char* out, size_t cap) override {
        return (size_t)snprintf(out, cap, "BruceLink stub\nBruce 1.17\nStickS3");
    }
    size_t wifiScan(char* out, size_t cap) override {
        return (size_t)snprintf(out, cap, "0 networks (stub)");
    }
    bool captureBackend(const bl_capture_backend_t** be, void** ctx) override {
        *be = &STUB_CAP;
        *ctx = nullptr;
        return true;
    }
    int activeOp(uint16_t, const uint8_t*, uint8_t, uint32_t) override {
        return 0; // stub: no transmission
    }
    uint32_t nowMs() override {
        return millis();
    }
    void randomBytes(uint8_t* out, size_t len) override {
        esp_fill_random(out, len);
    }
};

static const uint8_t DEMO_PSK[BL_KEY_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
    0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};

static BruceLinkBridge g_bridge;
static NimBleCentralTransport g_transport;
static StubBackend g_backend;

void setup() {
    Serial.begin(115200);
    g_transport.begin(&g_bridge, DEMO_PSK, &g_backend);
    g_transport.startScan(0);
}

void loop() {
    g_transport.loop();
    delay(10);
}
