/*
 * BruceLink module for Bruce 1.17: run the bridge from Bruce's menu.
 *
 * Lives in the Bruce tree at src/modules/brucelink/ (installed by
 * tools/install_into_bruce.py). Two menu entries under Config > Advanced:
 *   - "Toggle BruceLink", modelled on Bruce's own "Toggle BLE API": starts
 *     the bridge with the stored pairing key, or stops it;
 *   - "BruceLink pairing": pair with a Flipper (ADR 0005) or forget the
 *     pairing.
 *
 * While running, a dedicated task owns the bridge and the NimBLE central
 * transport: it scans for a Flipper advertising the BruceLink service,
 * connects, and serves authenticated requests (normal mode) or runs a
 * pairing attempt (pairing mode). Stopping it drops the link (which halts
 * captures and active ops) and releases the BLE stack.
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
    kModuleNotPaired, /* no stored pairing key: pair first */
};

/* Normal mode with the stored pairing key. */
ModuleStartResult moduleStart();
/* False if the bridge task did not stop in time; it is then still running
 * (or still stopping) and moduleRunning() stays true. */
bool moduleStop();
bool moduleRunning();
/* True while a Flipper is linked (BLE link up; see STATUS for the session). */
bool moduleLinked();

} // namespace brucelink

/* Menu entry points (Config > Advanced). */
void toggleBruceLink();
void bruceLinkPairingMenu();
