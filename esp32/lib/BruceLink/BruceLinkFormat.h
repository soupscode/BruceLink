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
 * Measured on the StickS3 (Bruce 1.17, 2026-11-23):
 *   - with 48 kB of internal RAM free, a full Arduino Wi-Fi start ran out
 *     part-way through esp_wifi_init ("Expected to init 4 rx buffer, actual
 *     is 2", ESP_ERR_NO_MEM). The driver could not be torn down again, the
 *     reply never left the device, and the bridge was dead until reboot;
 *   - with BLE up and the Flipper linked, about 62 kB is free and the
 *     largest DMA-capable block is about 32 kB.
 * So Wi-Fi is only started when it is likely to fit; otherwise the request
 * is refused with the numbers and the link survives. BruceLink starts the
 * driver with reduced buffers (BruceLinkWifi.cpp), which costs less than
 * the start that failed, but that cost has not been measured on hardware
 * yet: the threshold sits just above the known failure point and below
 * what is available, and is to be set from measurement.
 *
 * free_internal: free internal (non-PSRAM) heap in bytes.
 * largest_dma:   largest contiguous DMA-capable block in bytes.
 */
static const size_t kWifiStartMinFreeInternal = 52u * 1024u;
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
