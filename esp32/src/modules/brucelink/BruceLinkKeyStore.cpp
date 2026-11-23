/*
 * BruceLink — the StickS3's copy of the pairing key (see the header).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkKeyStore.h"

#include <Preferences.h>
#include <string.h>

extern "C" {
#include <bl_crypto.h>
#include <bl_demo_key.h>
}

namespace brucelink {

static const char* const kNamespace = "brucelink";
static const char* const kKeyPsk = "psk";
static const char* const kKeyPeer = "peer";
static const char* const kKeyVersion = "v";
static const uint8_t kFormat = 1;

static bool usable(const uint8_t psk[BL_KEY_LEN]) {
    static const uint8_t zero[BL_KEY_LEN] = {0};
    /* An all-zero key is what a wiped buffer looks like; the demo key is
     * public. Neither may ever authenticate a pairing. */
    return !bl_ct_equal(psk, zero, BL_KEY_LEN) && !bl_ct_equal(psk, BRUCELINK_DEMO_PSK, BL_KEY_LEN);
}

bool keyStoreLoad(uint8_t psk[BL_KEY_LEN], char peer[BL_PAIR_NAME_MAX + 1]) {
    peer[0] = '\0';
    Preferences p;
    if(!p.begin(kNamespace, true)) return false; // read-only; fails if never written
    bool ok = p.getUChar(kKeyVersion, 0) == kFormat && p.getBytesLength(kKeyPsk) == BL_KEY_LEN &&
              p.getBytes(kKeyPsk, psk, BL_KEY_LEN) == BL_KEY_LEN;
    if(ok) {
        size_t n = p.getString(kKeyPeer, peer, BL_PAIR_NAME_MAX + 1);
        if(n == 0) peer[0] = '\0';
        peer[BL_PAIR_NAME_MAX] = '\0';
    }
    p.end();
    if(ok && !usable(psk)) ok = false;
    if(!ok) bl_secure_zero(psk, BL_KEY_LEN);
    return ok;
}

bool keyStoreSave(const uint8_t psk[BL_KEY_LEN], const char* peer) {
    if(!usable(psk)) return false;
    Preferences p;
    if(!p.begin(kNamespace, false)) return false;
    bool ok = p.putBytes(kKeyPsk, psk, BL_KEY_LEN) == BL_KEY_LEN;
    char name[BL_PAIR_NAME_MAX + 1] = {0};
    if(peer != nullptr) strncpy(name, peer, BL_PAIR_NAME_MAX);
    ok = ok && p.putString(kKeyPeer, name) == strlen(name);
    ok = ok && p.putUChar(kKeyVersion, kFormat) == 1;
    if(!ok) p.clear(); // never leave half a pairing behind
    p.end();
    return ok;
}

void keyStoreForget() {
    Preferences p;
    if(!p.begin(kNamespace, false)) return;
    p.clear();
    p.end();
}

bool keyStoreInfo(char peer[BL_PAIR_NAME_MAX + 1], uint8_t key_id[BL_PAIR_KEY_ID_LEN]) {
    uint8_t psk[BL_KEY_LEN];
    if(!keyStoreLoad(psk, peer)) return false;
    bl_pair_key_id(psk, key_id);
    bl_secure_zero(psk, sizeof(psk));
    return true;
}

} // namespace brucelink
#endif // !LITE_VERSION
