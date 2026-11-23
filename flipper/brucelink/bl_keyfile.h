/*
 * BruceLink Flipper app — the stored pairing key.
 *
 * One file in the app's data folder (SD card). The key is encrypted with
 * this Flipper's device-unique key from the secure enclave, the way the
 * stock U2F app protects its keys: a copy of the SD card is useless on
 * another Flipper, and the file never holds the key in clear. A key id
 * computed from the decrypted key is checked on load, so a file from a
 * different Flipper (or a damaged one) is reported, not used. ADR 0005.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "../../shared/brucelink_core/src/bl_pair.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BlKeyfileOk,
    BlKeyfileNone, /* never paired (or forgotten) */
    BlKeyfileForeign, /* the file decrypts to a different key: another Flipper's, or damaged */
    BlKeyfileError, /* unreadable file or enclave error */
} BlKeyfileResult;

BlKeyfileResult bl_keyfile_load(
    uint8_t psk[BL_KEY_LEN],
    uint8_t key_id[BL_PAIR_KEY_ID_LEN],
    char peer[BL_PAIR_NAME_MAX + 1]);

bool bl_keyfile_save(const uint8_t psk[BL_KEY_LEN], const char* peer);

bool bl_keyfile_forget(void);
