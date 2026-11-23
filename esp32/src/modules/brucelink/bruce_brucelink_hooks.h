/*
 * Thin headless hooks the BruceLink patch adds to Bruce 1.17.
 *
 * These are the ONLY new behaviours BruceLink needs from Bruce; each
 * wraps Bruce's existing code so a remote caller can drive it without the
 * TFT/button UI loop. Keep the bodies minimal and in Bruce's own style.
 * Rationale + exact upstream call sites: docs/decisions/0003-capture-and-safety.md
 * and docs/HARDWARE_TESTING.md. See PATCH.md for the diff plan.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t packets;
    uint32_t eapol;
    uint32_t handshakes;
    uint32_t beacons;
    uint32_t deauth;
    uint32_t free_bytes;
    bool storage_full;
} BruceSnifferStatus;

/* Headless capture control. Implemented in the patch by lifting the radio
 * + file setup out of sniffer_setup() (sniffer.cpp) WITHOUT the UI loop:
 *   start: ensureWifiPlatform + esp_wifi AP/promiscuous + set_cb +
 *          set_channel + sniffer_prepare_storage + sniffer_set_mode.
 *   stop:  esp_wifi_set_promiscuous(false) + stop + rx_cb(NULL) +
 *          sniffer_wait_for_flush + closeRawFile/closeDeauthFile
 *          (today static — the patch exposes them or adds sniffer_stop()).
 * mode: 0=handshake-only, 1=raw(full), 2=deauth-pcap (matches
 *       bl_capture_mode_t). Returns false if the mode can't run (e.g. raw
 *       without SD). */
bool bruce_brucelink_sniffer_start(int mode, uint8_t channel);
bool bruce_brucelink_sniffer_set_channel(uint8_t channel);
bool bruce_brucelink_sniffer_status(BruceSnifferStatus* out);
void bruce_brucelink_sniffer_stop(void);
bool bruce_brucelink_raw_mode_available(void); /* == sniffer_full_mode_available() */

/* Active op: forwards to Bruce's existing send_raw_frame/wsl_bypasser
 * (wifi_atks.cpp). target is a 6-byte BSSID; returns frames sent or <0.
 * BruceLink has already enforced the remote safety gate before calling. */
int bruce_brucelink_deauth(const uint8_t* target_bssid, uint8_t channel);

#ifdef __cplusplus
}
#endif
