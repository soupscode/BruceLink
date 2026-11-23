/*
 * BruceLink module for Bruce 1.17 (see BruceLinkModule.h).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkModule.h"

#include "BruceBackendImpl.h"

#include <NimBleCentralTransport.h> // lib/BruceLink

extern "C" {
#include <bl_demo_key.h>
}

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <core/display.h> // displayInfo / displayError
#include <core/radio_mem.h> // radioHasMemForBle
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace brucelink {

/* The bridge task runs the session handshake (X25519: ~1.2 KB frame) and
 * handlers that stage one 512-byte payload plus a 540-byte frame, on top
 * of NimBLE client calls and a Wi-Fi scan. */
static const uint32_t kTaskStackBytes = 12 * 1024;
static const UBaseType_t kTaskPriority = 1; // same as Arduino's loopTask
static const TickType_t kLoopPeriod = pdMS_TO_TICKS(10);

static BruceLinkBridge g_bridge;
static NimBleCentralTransport g_transport;
static BruceBackendImpl g_backend;

static TaskHandle_t g_task = nullptr;
static SemaphoreHandle_t g_stopped = nullptr; // given by the task as it exits
static volatile bool g_run = false;
static volatile bool g_linked = false;

static void bridgeTask(void*) {
    while(g_run) {
        g_transport.loop();
        g_linked = g_transport.connected();
        vTaskDelay(kLoopPeriod);
    }
    // Shut down on the task that owns the bridge: the link loss reaches the
    // bridge first (captures stop, active ops halt), then NimBLE is released.
    g_transport.end();
    g_linked = false;
    xSemaphoreGive(g_stopped);
    vTaskDelete(nullptr);
}

ModuleStartResult moduleStart() {
    if(g_task != nullptr) return kModuleAlreadyRunning; // includes "still stopping"
    // One BLE stack, one owner: Bruce's BLE API, BadBLE, scanners etc. each
    // init/deinit NimBLE themselves.
    if(NimBLEDevice::isInitialized()) return kModuleBleBusy;
    if(!radioHasMemForBle()) return kModuleNoMemory;
    if(g_stopped == nullptr) g_stopped = xSemaphoreCreateBinary();
    if(g_stopped == nullptr) return kModuleNoMemory;

    // PUBLIC DEMO KEY until key provisioning exists (TASKS M5.3). A bridge
    // holding it serves passive requests only: the bridge core refuses every
    // active op (see BruceLinkBridge::begin).
    g_transport.begin(&g_bridge, BRUCELINK_DEMO_PSK, &g_backend);
    g_transport.startScan(0);

    g_run = true;
    if(xTaskCreate(bridgeTask, "brucelink", kTaskStackBytes, nullptr, kTaskPriority, &g_task) !=
       pdPASS) {
        g_run = false;
        g_task = nullptr;
        g_transport.end();
        return kModuleNoMemory;
    }
    return kModuleStarted;
}

bool moduleStop() {
    if(g_task == nullptr) return true;
    g_run = false;
    // A Wi-Fi scan or a BLE connect attempt in progress has to finish first
    // (bounded: NimBleCentralTransport::kConnectTimeoutMs, a scan of a few
    // seconds). If the task still has not exited, it stays registered as
    // running, so a second task is never started alongside it.
    if(xSemaphoreTake(g_stopped, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
    g_task = nullptr;
    return true;
}

bool moduleRunning() {
    return g_task != nullptr;
}

bool moduleLinked() {
    return g_linked;
}

} // namespace brucelink

void toggleBruceLink() {
    using namespace brucelink;
    if(moduleRunning()) {
        if(moduleStop()) displayInfo("BruceLink OFF", true);
        else displayError("BruceLink is still stopping, try again", true);
        return;
    }
    switch(moduleStart()) {
    case kModuleStarted:
        // Say what this build is: both ends share a published key and the
        // link is not encrypted, so it is for bring-up, not for trust.
        displayInfo("BruceLink ON > looking for Flipper (DEMO KEY, not secure)", true);
        break;
    case kModuleBleBusy: displayError("BruceLink: BLE in use, turn the other BLE feature off", true); break;
    case kModuleNoMemory: displayError("BruceLink: not enough memory for BLE", true); break;
    default: break;
    }
}
#endif // !LITE_VERSION
