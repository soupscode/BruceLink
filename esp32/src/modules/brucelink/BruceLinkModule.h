/*
 * BruceLink module for Bruce 1.17: start/stop the bridge from Bruce's menu.
 *
 * Lives in the Bruce tree at src/modules/brucelink/ (installed by
 * tools/install_into_bruce.py). Modelled on Bruce's own "Toggle BLE API"
 * (src/core/settings.cpp enableBLEAPI): one toggle, a status message, and
 * the work happens in the background while Bruce's UI keeps running.
 *
 * While running, a dedicated task owns the bridge and the NimBLE central
 * transport: it scans for a Flipper advertising the BruceLink service,
 * connects, and serves authenticated requests. Stopping it drops the link
 * (which halts captures and active ops) and releases the BLE stack.
 * SPDX-License-Identifier: MIT
 */
#pragma once

namespace brucelink {

/* Why a start was refused. */
enum ModuleStartResult {
    kModuleStarted = 0,
    kModuleAlreadyRunning,
    kModuleBleBusy, /* another Bruce feature holds the BLE stack */
    kModuleNoMemory, /* not enough contiguous DMA memory / task could not start */
};

ModuleStartResult moduleStart();
void moduleStop();
bool moduleRunning();
/* True while a Flipper is linked (BLE link up; see STATUS for the session). */
bool moduleLinked();

} // namespace brucelink

/* Menu entry point (next to "Toggle BLE API"): toggles the bridge and tells
 * the user the new state. */
void toggleBruceLink();
