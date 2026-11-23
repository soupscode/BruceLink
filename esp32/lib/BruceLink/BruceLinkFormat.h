/*
 * BruceLink ESP32-S3 bridge — response text formatting.
 *
 * Portable (no Arduino/Bruce deps) so it is host-tested; the Bruce glue
 * uses it to render scan results into a bounded buffer.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>

extern "C" {
#include "bl_textbuf.h"
}

namespace brucelink {

/* IEEE 802.11 SSIDs are at most 32 octets. */
static const size_t kSsidMaxLen = 32;

/*
 * Append one scanned AP as "<ssid> <rssi>dBm ch<channel>\n".
 * The SSID is untrusted broadcast data: it is cut at kSsidMaxLen bytes and
 * control characters (incl. '\n') become '?', so a crafted SSID cannot
 * inject extra lines. An empty SSID renders as "<hidden>". The line is
 * appended whole or not at all; returns false (buffer full) otherwise.
 */
bool formatWifiApLine(bl_textbuf_t* tb, const char* ssid, int rssi, int channel);

/*
 * Wi-Fi start guard for a device whose BLE link is the control channel.
 *
 * Measured on the StickS3 (Bruce 1.17, 2026-11-23): with NimBLE up,
 * esp_wifi_init() ran out of internal RAM part-way ("Expected to init 4 rx
 * buffer, actual is 2", ESP_ERR_NO_MEM). The driver could not be torn down
 * again, the scan reply never left the device, and the bridge was dead
 * until reboot. So Wi-Fi is only started when it is very likely to fit;
 * otherwise the request is refused with the numbers, and the link survives.
 *
 * free_internal: free internal (non-PSRAM) heap in bytes.
 * largest_dma:   largest contiguous DMA-capable block in bytes.
 * Both thresholds are estimates until the real cost of Wi-Fi start has been
 * measured on the device (docs/HARDWARE_TESTING.md).
 */
static const size_t kWifiStartMinFreeInternal = 80u * 1024u;
static const size_t kWifiStartMinDmaBlock = 16u * 1024u;

/* True if Wi-Fi may be started. Wi-Fi that is already up costs nothing more. */
bool wifiStartAllowed(bool wifi_already_up, size_t free_internal, size_t largest_dma);

/* Append the refusal shown on the Flipper, e.g.
 *   "Wi-Fi can't start with BLE on
free 52k need 80k
block 20k need 16k"
 * Appended whole or not at all. */
bool formatWifiNoMemory(bl_textbuf_t* tb, size_t free_internal, size_t largest_dma);

} // namespace brucelink
