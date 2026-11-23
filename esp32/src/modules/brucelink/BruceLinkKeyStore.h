/*
 * BruceLink — the StickS3's copy of the pairing key (Bruce 1.17).
 *
 * Kept in NVS (ESP32 Preferences, namespace "brucelink"), as Bruce keeps
 * its own U2F secrets: not in bruce.conf (Bruce prints that to serial) and
 * not on LittleFS (Bruce's file browser and web UI can read that). Flash
 * encryption is off on this build, so anyone with the device and a USB
 * cable can still read it; see ADR 0005.
 *
 * Flashing the merged image (offset 0x0) erases NVS and with it the
 * pairing; flashing only the app (offset 0x10000) keeps it.
 *
 * Builds only inside the Bruce tree. Call from one task (the UI task).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include <bl_pair.h>
}

namespace brucelink {

/* Load the stored pairing. False if there is none or it is unusable.
 * peer receives the Flipper's name (NUL-terminated, may be empty). */
bool keyStoreLoad(uint8_t psk[BL_KEY_LEN], char peer[BL_PAIR_NAME_MAX + 1]);
/* Replace the stored pairing. */
bool keyStoreSave(const uint8_t psk[BL_KEY_LEN], const char* peer);
/* Remove it. */
void keyStoreForget();
/* True if a usable pairing is stored (its name and key id for display). */
bool keyStoreInfo(char peer[BL_PAIR_NAME_MAX + 1], uint8_t key_id[BL_PAIR_KEY_ID_LEN]);

} // namespace brucelink
