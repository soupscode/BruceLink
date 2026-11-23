/*
 * BruceLink headless hooks — "not available" implementations.
 *
 * bruce_brucelink_hooks.h declares the few headless entry points BruceLink
 * needs from Bruce's sniffer and transmit code. Implementing them means
 * changing Bruce's sniffer.cpp (PATCH.md step 1, task M5.2), which has not
 * been done yet. Until then this file lets the bridge build and link inside
 * Bruce with every capture and active operation reporting "not available":
 *   - capture start  -> refused (the Flipper sees `no_resource`)
 *   - active op      -> error, nothing is transmitted
 * Device info, Wi-Fi scan, capabilities and status do not use these hooks.
 *
 * Remove this file from the build when the real hooks land (it defines the
 * same symbols).
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "bruce_brucelink_hooks.h"

#include <string.h>

bool bruce_brucelink_sniffer_start(int mode, uint8_t channel) {
    (void)mode;
    (void)channel;
    return false;
}

bool bruce_brucelink_sniffer_set_channel(uint8_t channel) {
    (void)channel;
    return false;
}

bool bruce_brucelink_sniffer_status(BruceSnifferStatus* out) {
    if(out != NULL) memset(out, 0, sizeof(*out));
    return false;
}

void bruce_brucelink_sniffer_stop(void) {
}

bool bruce_brucelink_raw_mode_available(void) {
    return false;
}

int bruce_brucelink_deauth(const uint8_t* target_bssid, uint8_t channel, uint32_t max_frames) {
    (void)target_bssid;
    (void)channel;
    (void)max_frames;
    return -1; /* no transmit hook in this build: nothing is sent */
}
#endif // !LITE_VERSION
