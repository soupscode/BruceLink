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

bool wifiStartAllowed(bool wifi_already_up, size_t free_internal, size_t largest_dma) {
    if(wifi_already_up) return true;
    return free_internal >= kWifiStartMinFreeInternal && largest_dma >= kWifiStartMinDmaBlock;
}

bool formatWifiNoMemory(bl_textbuf_t* tb, size_t free_internal, size_t largest_dma) {
    /* Built separately so it is appended atomically. Four numbers of at
     * most 20 digits plus ~55 characters of text. */
    char msg[160];
    bl_textbuf_t mt;
    bl_tb_init(&mt, msg, sizeof(msg));
    bool ok = bl_tb_str(&mt, "Wi-Fi can't start with BLE on\nfree ") &&
              bl_tb_uint(&mt, (unsigned long)(free_internal / 1024u)) &&
              bl_tb_str(&mt, "k need ") &&
              bl_tb_uint(&mt, (unsigned long)(kWifiStartMinFreeInternal / 1024u)) &&
              bl_tb_str(&mt, "k\nblock ") &&
              bl_tb_uint(&mt, (unsigned long)(largest_dma / 1024u)) && bl_tb_str(&mt, "k need ") &&
              bl_tb_uint(&mt, (unsigned long)(kWifiStartMinDmaBlock / 1024u)) && bl_tb_str(&mt, "k");
    if(!ok) return false;
    return bl_tb_str(tb, msg);
}

} // namespace brucelink
