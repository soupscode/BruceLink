/*
 * BruceLink module for Bruce 1.17 (see BruceLinkModule.h).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkModule.h"

#include "BruceBackendImpl.h"
#include "BruceLinkJobs.h"
#include "BruceLinkKeyStore.h"

#include <NimBleCentralTransport.h> // lib/BruceLink

extern "C" {
#include <bl_crypto.h>
#include <bl_pair.h>
}

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <core/display.h> // displayInfo / displayError / displayMessage / loopOptions
#include <core/radio_mem.h> // radioHasMemForBle
#include <esp_system.h> // esp_restart
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <globals.h>
#include <interface.h> // InputHandler
#include <stdio.h>
#include <string.h>

namespace brucelink {

/* The bridge task runs the session handshake or a pairing attempt (X25519:
 * ~1.2 KB frame) and handlers that stage one 512-byte payload plus a
 * 540-byte frame, on top of NimBLE client calls and a Wi-Fi scan. Measured
 * peak so far: 4.3 kB (handshake + Wi-Fi scan, stack-left in the log). */
static const uint32_t kTaskStackBytes = 10 * 1024;
static const UBaseType_t kTaskPriority = 1; // same as Arduino's loopTask
static const TickType_t kLoopPeriod = pdMS_TO_TICKS(10);
/* Pairing mode gives up if no Flipper has shown up in this long. */
static const uint32_t kPairingFindMs = 180000;

static BruceLinkBridge g_bridge;
static NimBleCentralTransport g_transport;
static BruceBackendImpl g_backend;

static TaskHandle_t g_task = nullptr;
static SemaphoreHandle_t g_stopped = nullptr; // given by the task as it exits
static volatile bool g_run = false;
static volatile bool g_linked = false;
static volatile bool g_pairing_mode = false;

/* ---- pairing: what the bridge task publishes for the UI task ---------- */

struct PairView {
    uint32_t attempt; /* bumped for every new link in pairing mode */
    bl_pair_state_t state;
    bool have_code;
    uint32_t code;
    char peer[BL_PAIR_NAME_MAX + 1];
    bl_pair_fail_t fail;
    uint8_t peer_reason;
    bool result_ready; /* key below is valid; the UI takes it */
    uint8_t key[BL_KEY_LEN];
    uint8_t key_id[BL_PAIR_KEY_ID_LEN];
};
static portMUX_TYPE g_pair_mux = portMUX_INITIALIZER_UNLOCKED;
static PairView g_pv;
/* UI -> task: the user's answer for attempt g_answer_attempt (-1: none). */
static volatile int8_t g_answer = -1;
static volatile uint32_t g_answer_attempt = 0;
/* UI -> task: leave pairing mode and run sessions with g_switch_key, keeping
 * BLE up (see switchToNormal). The key is guarded by g_pair_mux. */
static volatile bool g_switch_pending = false;
static uint8_t g_switch_key[BL_KEY_LEN];

/* On the bridge task. */
static void applySwitch() {
    uint8_t key[BL_KEY_LEN];
    portENTER_CRITICAL(&g_pair_mux);
    memcpy(key, g_switch_key, BL_KEY_LEN);
    bl_secure_zero(g_switch_key, sizeof(g_switch_key));
    bl_secure_zero(&g_pv, sizeof(g_pv));
    portEXIT_CRITICAL(&g_pair_mux);
    // A pairing link still up is dropped without a cooldown; the next link
    // is a session with this key (host-tested in test_bridge.cpp).
    g_bridge.endPairing();
    g_bridge.setKey(key);
    bl_secure_zero(key, sizeof(key));
    g_pairing_mode = false;
    g_switch_pending = false;
    logStep("module: pairing mode -> normal, BLE kept up");
}

static void pairingStep(bool linked_now, bool was_linked) {
    if(linked_now && !was_linked) {
        portENTER_CRITICAL(&g_pair_mux);
        g_pv.attempt++;
        portEXIT_CRITICAL(&g_pair_mux);
    }
    const int8_t answer = g_answer;
    if(answer >= 0) {
        g_answer = -1;
        if(g_answer_attempt == g_pv.attempt) g_bridge.pairAnswer(answer == 1);
    }
    PairView v;
    portENTER_CRITICAL(&g_pair_mux);
    const bool already = g_pv.result_ready;
    portEXIT_CRITICAL(&g_pair_mux);
    uint8_t key[BL_KEY_LEN];
    uint8_t kid[BL_PAIR_KEY_ID_LEN];
    const bool took = !already && g_bridge.pairState() == BL_PS_DONE &&
                      g_bridge.pairTakeResult(key, kid);
    v.state = g_bridge.pairState();
    v.have_code = g_bridge.pairCode(&v.code);
    strncpy(v.peer, g_bridge.pairPeerName(), BL_PAIR_NAME_MAX);
    v.peer[BL_PAIR_NAME_MAX] = '\0';
    v.fail = g_bridge.pairFail();
    v.peer_reason = g_bridge.pairPeerReason();
    portENTER_CRITICAL(&g_pair_mux);
    g_pv.state = v.state;
    if(v.have_code) { // keep the last code on screen after the key is taken
        g_pv.have_code = true;
        g_pv.code = v.code;
    } else if(v.state != BL_PS_DONE) {
        g_pv.have_code = false;
    }
    if(v.peer[0] != '\0') memcpy(g_pv.peer, v.peer, sizeof(v.peer));
    g_pv.fail = v.fail;
    g_pv.peer_reason = v.peer_reason;
    if(took) {
        memcpy(g_pv.key, key, BL_KEY_LEN);
        memcpy(g_pv.key_id, kid, BL_PAIR_KEY_ID_LEN);
        g_pv.result_ready = true;
    }
    portEXIT_CRITICAL(&g_pair_mux);
    bl_secure_zero(key, sizeof(key));
}

/* A job was accepted (ADR 0006): let the reply leave, release BLE so the
 * Flipper sees the link drop, and restart into the job. */
static const uint32_t kJobReplyFlushMs = 500;
static void restartForJob() {
    logStep("job: accepted, restarting into it");
    vTaskDelay(pdMS_TO_TICKS(kJobReplyFlushMs));
    g_transport.end();
    esp_restart();
}

static void bridgeTask(void*) {
    bool was_linked = false;
    while(g_run) {
        if(g_switch_pending) applySwitch();
        g_transport.loop();
        if(jobRestartRequested()) restartForJob();
        const bool linked = g_transport.connected();
        if(linked != was_linked) logStep(linked ? "module: Flipper linked" : "module: link down");
        if(g_pairing_mode) pairingStep(linked, was_linked);
        g_linked = linked;
        was_linked = linked;
        vTaskDelay(kLoopPeriod);
    }
    // Shut down on the task that owns the bridge: the link loss reaches the
    // bridge first (captures stop, active ops halt), then NimBLE is released.
    g_transport.end();
    g_bridge.endPairing();
    g_linked = false;
    xSemaphoreGive(g_stopped);
    vTaskDelete(nullptr);
}

/* Bring BLE up and start the task. psk == nullptr: pairing mode. */
static ModuleStartResult startTask(const uint8_t* psk) {
    if(g_task != nullptr) return kModuleAlreadyRunning; // includes "still stopping"
    // One BLE stack, one owner: Bruce's BLE API, BadBLE, scanners etc. each
    // init/deinit NimBLE themselves.
    if(NimBLEDevice::isInitialized()) return kModuleBleBusy;
    if(!radioHasMemForBle()) {
        logStep("module: not enough memory for BLE (needs a 48 kB dma block)");
        return kModuleNoMemory;
    }
    if(g_stopped == nullptr) g_stopped = xSemaphoreCreateBinary();
    if(g_stopped == nullptr) return kModuleNoMemory;

    g_switch_pending = false;
    portENTER_CRITICAL(&g_pair_mux);
    bl_secure_zero(g_switch_key, sizeof(g_switch_key));
    portEXIT_CRITICAL(&g_pair_mux);
    g_transport.begin(&g_bridge, psk, &g_backend);
    g_pairing_mode = psk == nullptr;
    if(g_pairing_mode) {
        portENTER_CRITICAL(&g_pair_mux);
        bl_secure_zero(&g_pv, sizeof(g_pv));
        portEXIT_CRITICAL(&g_pair_mux);
        g_answer = -1;
        g_bridge.beginPairing(DEVICE_NAME);
    }
    g_transport.startScan(0);
    logStep(g_pairing_mode ? "module: BLE up, pairing mode" : "module: BLE up, scanning for the Flipper");

    g_run = true;
    if(xTaskCreate(bridgeTask, "brucelink", kTaskStackBytes, nullptr, kTaskPriority, &g_task) !=
       pdPASS) {
        g_run = false;
        g_task = nullptr;
        g_transport.end();
        g_bridge.endPairing();
        return kModuleNoMemory;
    }
    return kModuleStarted;
}

ModuleStartResult moduleStart() {
    if(g_task != nullptr) return kModuleAlreadyRunning;
    logStep("module: start requested");
    uint8_t psk[BL_KEY_LEN];
    char peer[BL_PAIR_NAME_MAX + 1];
    if(!keyStoreLoad(psk, peer)) return kModuleNotPaired;
    const ModuleStartResult r = startTask(psk); // the bridge keeps its own copy
    bl_secure_zero(psk, sizeof(psk));
    return r;
}

bool moduleStop() {
    if(g_task == nullptr) return true;
    g_run = false;
    // A Wi-Fi scan or a BLE connect attempt in progress has to finish first
    // (bounded: NimBleCentralTransport::kConnectTimeoutMs, a scan of a few
    // seconds). If the task still has not exited, it stays registered as
    // running, so a second task is never started alongside it.
    if(xSemaphoreTake(g_stopped, pdMS_TO_TICKS(15000)) != pdTRUE) return false;
    // The task deletes itself right after giving the semaphore; its stack
    // and NimBLE's host task are freed by the idle task. Let that happen
    // before anyone starts a radio again (BLE needs a 48 kB block).
    vTaskDelay(pdMS_TO_TICKS(100));
    g_task = nullptr;
    g_pairing_mode = false;
    g_switch_pending = false;
    portENTER_CRITICAL(&g_pair_mux);
    bl_secure_zero(&g_pv, sizeof(g_pv));
    bl_secure_zero(g_switch_key, sizeof(g_switch_key));
    portEXIT_CRITICAL(&g_pair_mux);
    logStep("module: stopped, BLE released");
    return true;
}

bool moduleRunning() {
    return g_task != nullptr;
}

bool moduleLinked() {
    return g_linked;
}

/* ---- pairing screen (UI task) ----------------------------------------- */

static const char* failText(bl_pair_fail_t f, uint8_t peer_reason) {
    switch(f) {
    case BL_PF_REJECTED: return "You said the codes differ";
    case BL_PF_PEER_REJECTED:
        switch(peer_reason) {
        case BL_PAIR_ABORT_REJECTED: return "The Flipper said the codes differ";
        case BL_PAIR_ABORT_TIMEOUT: return "The Flipper timed out";
        case BL_PAIR_ABORT_MISMATCH: return "Key check failed (interference?)";
        default: return "The Flipper cancelled";
        }
    case BL_PF_MISMATCH: return "Key check failed (interference?)";
    case BL_PF_CRYPTO: return "Bad key from the Flipper";
    case BL_PF_TIMEOUT: return "Timed out";
    case BL_PF_CANCELLED: return "Link lost";
    default: return "Failed";
    }
}

static void drawWaiting(const char* last_failure) {
    drawMainBorderWithTitle("BruceLink pairing");
    padprintln("");
    padprintln("On the Flipper:");
    padprintln("BruceLink > Pair");
    padprintln("");
    if(last_failure != nullptr) {
        padprintln("Last try:");
        padprintln(last_failure);
    } else {
        padprintln("Waiting for it...");
    }
    printFootnote("ESC: cancel");
}

static void formatCode(uint32_t code, char out[8]) {
    snprintf(out, 8, "%03u %03u", (unsigned)(code / 1000u), (unsigned)(code % 1000u));
}

/* "Not enough memory" with the figures: BLE needs one free block this big.
 * A reboot gives the heap back in one piece. */
static void showNoMemory() {
    char msg[80];
    snprintf(msg, sizeof(msg), "Not enough memory for BLE: largest block %u kB, needs %u. Reboot, then retry",
             (unsigned)(radioLargestDmaBlock() / 1024), (unsigned)(RADIO_BLE_MIN_DMA_BLOCK / 1024));
    displayError(msg, true);
}

/* Leave pairing mode for normal sessions with `psk` without restarting BLE.
 * Stopping and starting again needs a fresh 48 kB DMA block, which the heap
 * may no longer have after a BLE session (hardware, 2026-11-23: paired, then
 * "not enough memory" on the restart). The bridge task applies it on its
 * next loop. */
static void switchToNormal(const uint8_t psk[BL_KEY_LEN]) {
    portENTER_CRITICAL(&g_pair_mux);
    memcpy(g_switch_key, psk, BL_KEY_LEN);
    portEXIT_CRITICAL(&g_pair_mux);
    g_switch_pending = true;
}

/* Returns true if a new pairing was stored. */
static bool runPairing() {
    const bool was_running = moduleRunning();
    if(was_running && !moduleStop()) {
        displayError("BruceLink is still stopping, try again", true);
        return false;
    }
    switch(startTask(nullptr)) {
    case kModuleStarted: break;
    case kModuleBleBusy: displayError("BLE in use, turn the other BLE feature off", true); return false;
    default: showNoMemory(); return false;
    }

    bool stored = false;
    char done_msg[64] = "";
    const char* error = nullptr;
    const char* last_failure = nullptr;
    uint32_t asked_attempt = 0; // 0: none asked yet
    uint32_t seen_attempt = 0;
    const uint32_t started = millis();
    drawWaiting(nullptr);
    while(!check(EscPress) && !returnToMenu) {
        InputHandler();
        wakeUpScreen();
        PairView v;
        portENTER_CRITICAL(&g_pair_mux);
        v = g_pv;
        portEXIT_CRITICAL(&g_pair_mux);

        if(v.result_ready) {
            portENTER_CRITICAL(&g_pair_mux);
            bl_secure_zero(g_pv.key, sizeof(g_pv.key));
            g_pv.result_ready = false;
            portEXIT_CRITICAL(&g_pair_mux);
            stored = keyStoreSave(v.key, v.peer);
            bl_secure_zero(v.key, sizeof(v.key));
            if(stored) {
                snprintf(done_msg, sizeof(done_msg), "Paired with %s, key %02X%02X", v.peer, v.key_id[0],
                         v.key_id[1]);
            } else {
                error = "Paired, but saving the key failed";
            }
            break;
        }
        bl_secure_zero(v.key, sizeof(v.key));

        if(v.attempt != seen_attempt) {
            seen_attempt = v.attempt;
            drawWaiting(last_failure);
        }
        if(v.have_code && v.state == BL_PS_COMPARE && asked_attempt != v.attempt) {
            asked_attempt = v.attempt;
            char code[8];
            formatCode(v.code, code);
            char msg[96];
            snprintf(msg, sizeof(msg), "Code %s\nFlipper: %s\nSame code on\nthe Flipper?", code,
                     v.peer[0] ? v.peer : "(no name)");
            tft.fillScreen(bruceConfig.bgColor);
            // "No" is the default: pressing select without looking refuses.
            const int8_t choice = displayMessage(msg, "No", nullptr, "Yes", bruceConfig.priColor);
            g_answer_attempt = v.attempt;
            g_answer = choice == 1 ? 1 : 0;
            drawMainBorderWithTitle("BruceLink pairing");
            padprintln("");
            padprintln(choice == 1 ? "Waiting for the Flipper" : "Refused");
            printFootnote("ESC: cancel");
        }
        if(v.state == BL_PS_FAILED && v.attempt != 0 && last_failure != failText(v.fail, v.peer_reason)) {
            last_failure = failText(v.fail, v.peer_reason);
            drawWaiting(last_failure);
        }
        if(v.attempt == 0 && millis() - started > kPairingFindMs) {
            error = "No Flipper in pairing mode found";
            break;
        }
        delay(20);
    }

    // Leave pairing mode. With a key to run (the new one, or the old one if
    // BruceLink was on before) BLE stays up; otherwise it is released.
    bool on = false;
    if(moduleRunning()) {
        uint8_t psk[BL_KEY_LEN];
        char peer[BL_PAIR_NAME_MAX + 1];
        if((stored || was_running) && keyStoreLoad(psk, peer)) {
            switchToNormal(psk);
            on = true;
        } else {
            moduleStop();
        }
        bl_secure_zero(psk, sizeof(psk));
    }
    if(stored) {
        displaySuccess(done_msg, true);
    } else if(error != nullptr) {
        displayError(error, true);
    }
    if(on) displayInfo("BruceLink ON > looking for Flipper (not encrypted)", true);
    return stored;
}

static void toggleModule() {
    if(moduleRunning()) {
        if(moduleStop()) displayInfo("BruceLink OFF", true);
        else displayError("BruceLink is still stopping, try again", true);
        return;
    }
    switch(moduleStart()) {
    case kModuleStarted:
        // Paired key, but frames are authenticated, not encrypted (M4.6).
        displayInfo("BruceLink ON > looking for Flipper (not encrypted)", true);
        break;
    case kModuleNotPaired: displayError("BruceLink: not paired. Pair with Flipper first", true); break;
    case kModuleBleBusy: displayError("BruceLink: BLE in use, turn the other BLE feature off", true); break;
    case kModuleNoMemory: showNoMemory(); break;
    default: break;
    }
}

static void pairWithFlipper(bool paired) {
    if(paired) {
        tft.fillScreen(bruceConfig.bgColor);
        if(displayMessage("Replace the current\npairing?", "No", nullptr, "Yes", bruceConfig.priColor) != 1) {
            return;
        }
    }
    runPairing();
}

static void forgetPairing() {
    tft.fillScreen(bruceConfig.bgColor);
    if(displayMessage("Forget the pairing?\nThe Flipper will need\nto pair again.", "No", nullptr, "Yes",
                      TFT_RED) != 1) {
        return;
    }
    if(moduleRunning()) moduleStop();
    keyStoreForget();
    displayInfo("Pairing forgotten", true);
}

} // namespace brucelink

void bruceLinkMenu() {
    using namespace brucelink;
    int index = 0;
    while(!returnToMenu) {
        char peer[BL_PAIR_NAME_MAX + 1];
        uint8_t kid[BL_PAIR_KEY_ID_LEN];
        const bool paired = keyStoreInfo(peer, kid);
        const bool running = moduleRunning();
        // Title line: on/off and which Flipper (the key id, never the key).
        char status[48];
        if(paired) {
            snprintf(status, sizeof(status), "BruceLink %s | %s %02X%02X", running ? "ON" : "OFF",
                     peer[0] ? peer : "Flipper", kid[0], kid[1]);
        } else {
            snprintf(status, sizeof(status), "BruceLink %s | not paired", running ? "ON" : "OFF");
        }

        // Rebuilt on every pass: the entries depend on the state above.
        std::vector<Option> opts;
        opts.push_back({running ? "Turn off" : "Turn on", []() { toggleModule(); }});
        opts.push_back({paired ? "Pair again" : "Pair with Flipper", [paired]() { pairWithFlipper(paired); }});
        if(paired) opts.push_back({"Forget pairing", []() { forgetPairing(); }});
        opts.push_back({"Main Menu", []() { returnToMenu = true; }});
        if(index >= (int)opts.size()) index = (int)opts.size() - 1;

        index = loopOptions(opts, MENU_TYPE_SUBMENU, status, index);
        if(index < 0) break; // Esc
    }
}
#endif // !LITE_VERSION
