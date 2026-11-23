/*
 * BruceLink — Wi-Fi scan next to a live BLE link (Bruce 1.17, StickS3).
 *
 * The BLE link is the control channel, so Wi-Fi has to come up while NimBLE
 * is running, and both radio stacks allocate from the same internal RAM.
 * Measured on the StickS3 (2026-11-23): 133 kB internal free at the menu,
 * about 62 kB with BLE up and the Flipper linked; a full Arduino Wi-Fi start
 * did not fit even in that and its failure took the bridge down.
 *
 * So a scan requested over BruceLink uses the driver directly:
 *   - if Bruce already has Wi-Fi up (WiFi.getMode() != NULL), scan through
 *     the Arduino API exactly as Bruce's own scan does; nothing is started;
 *   - if the driver is not initialised, bring it up with a reduced buffer
 *     configuration (no association or data traffic is needed to scan),
 *     scan, and release it again. Bruce's Wi-Fi state is left as found;
 *   - if something else holds the driver, refuse.
 * It never starts Wi-Fi unless the memory guard says it should fit.
 *
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#pragma once

extern "C" {
#include <bl_textbuf.h>
}

namespace brucelink {

/* Run a scan and write the reply text for the Flipper into tb:
 * "<n> networks, <ms> ms\n<ssid> <rssi>dBm ch<c>\n..." or a reason. */
void wifiScanInto(bl_textbuf_t* tb);

} // namespace brucelink
