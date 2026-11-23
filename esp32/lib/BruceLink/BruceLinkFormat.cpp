/*
 * BruceLink ESP32-S3 bridge — response text formatting.
 * SPDX-License-Identifier: MIT
 */
#include "BruceLinkFormat.h"

namespace brucelink {

bool formatWifiApLine(bl_textbuf_t* tb, const char* ssid, int rssi, int channel) {
    /* Build the line separately so it is appended atomically. Worst case:
     * 32 (ssid) + 1 + 11 (int) + 5 ("dBm ch") + 11 (int) + 1 + NUL < 64. */
    char line[64];
    bl_textbuf_t lt;
    bl_tb_init(&lt, line, sizeof(line));
    bool ok = (ssid == nullptr || ssid[0] == '\0') ? bl_tb_str(&lt, "<hidden>") :
                                                     bl_tb_sanitized(&lt, ssid, kSsidMaxLen);
    ok = ok && bl_tb_str(&lt, " ") && bl_tb_int(&lt, rssi) && bl_tb_str(&lt, "dBm ch") &&
         bl_tb_int(&lt, channel) && bl_tb_str(&lt, "\n");
    if(!ok) return false;
    return bl_tb_str(tb, line);
}

} // namespace brucelink
