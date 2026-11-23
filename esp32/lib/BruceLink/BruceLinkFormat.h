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

} // namespace brucelink
