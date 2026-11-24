/*
 * BruceLink Flipper app — the stored pairing key (see bl_keyfile.h).
 * SPDX-License-Identifier: MIT
 */
#include "bl_keyfile.h"

#include "../../shared/brucelink_core/src/bl_crypto.h"

#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <furi_hal_crypto.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "BruceLinkKey"

#define KEYFILE_PATH APP_DATA_PATH("pairing.key")
#define KEYFILE_TYPE "BruceLink pairing key"
#define KEYFILE_VERSION 1u
#define KEY_SLOT FURI_HAL_CRYPTO_ENCLAVE_UNIQUE_KEY_SLOT
#define IV_LEN 16u

/* AES with the device-unique key; 32 bytes is two blocks. */
static bool crypt(bool encrypt, const uint8_t iv[IV_LEN], const uint8_t* in, uint8_t* out) {
    if(!furi_hal_crypto_enclave_ensure_key(KEY_SLOT)) return false;
    if(!furi_hal_crypto_enclave_load_key(KEY_SLOT, iv)) return false;
    const bool ok = encrypt ? furi_hal_crypto_encrypt(in, out, BL_KEY_LEN) :
                              furi_hal_crypto_decrypt(in, out, BL_KEY_LEN);
    furi_hal_crypto_enclave_unload_key(KEY_SLOT);
    return ok;
}

BlKeyfileResult bl_keyfile_load(
    uint8_t psk[BL_KEY_LEN],
    uint8_t key_id[BL_PAIR_KEY_ID_LEN],
    char peer[BL_PAIR_NAME_MAX + 1]) {
    peer[0] = '\0';
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage_file_exists(storage, KEYFILE_PATH)) {
        furi_record_close(RECORD_STORAGE);
        return BlKeyfileNone;
    }
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    FuriString* str = furi_string_alloc();
    BlKeyfileResult result = BlKeyfileError;
    uint8_t iv[IV_LEN];
    uint8_t enc[BL_KEY_LEN];
    uint8_t want_id[BL_PAIR_KEY_ID_LEN];
    uint32_t version = 0;
    do {
        if(!flipper_format_file_open_existing(ff, KEYFILE_PATH)) break;
        if(!flipper_format_read_header(ff, str, &version)) break;
        if(strcmp(furi_string_get_cstr(str), KEYFILE_TYPE) != 0 || version != KEYFILE_VERSION) break;
        if(!flipper_format_read_string(ff, "Peer", str)) break;
        strncpy(peer, furi_string_get_cstr(str), BL_PAIR_NAME_MAX);
        peer[BL_PAIR_NAME_MAX] = '\0';
        if(!flipper_format_read_hex(ff, "Key ID", want_id, sizeof(want_id))) break;
        if(!flipper_format_read_hex(ff, "IV", iv, sizeof(iv))) break;
        if(!flipper_format_read_hex(ff, "Key", enc, sizeof(enc))) break;
        if(!crypt(false, iv, enc, psk)) {
            FURI_LOG_E(TAG, "decrypt failed");
            break;
        }
        bl_pair_key_id(psk, key_id);
        if(!bl_ct_equal(key_id, want_id, sizeof(want_id))) {
            FURI_LOG_W(TAG, "key file belongs to another Flipper or is damaged");
            result = BlKeyfileForeign;
            break;
        }
        result = BlKeyfileOk;
    } while(false);
    if(result != BlKeyfileOk) {
        bl_secure_zero(psk, BL_KEY_LEN);
        if(result != BlKeyfileForeign) peer[0] = '\0';
    }
    bl_secure_zero(enc, sizeof(enc));
    furi_string_free(str);
    flipper_format_free(ff);
    furi_record_close(RECORD_STORAGE);
    return result;
}

bool bl_keyfile_save(const uint8_t psk[BL_KEY_LEN], const char* peer) {
    uint8_t iv[IV_LEN];
    uint8_t enc[BL_KEY_LEN];
    uint8_t key_id[BL_PAIR_KEY_ID_LEN];
    furi_hal_random_fill_buf(iv, sizeof(iv));
    if(!crypt(true, iv, psk, enc)) {
        FURI_LOG_E(TAG, "encrypt failed");
        return false;
    }
    bl_pair_key_id(psk, key_id);
    char name[BL_PAIR_NAME_MAX + 1] = {0};
    if(peer != NULL) strncpy(name, peer, BL_PAIR_NAME_MAX);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, APP_DATA_PATH(""));
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    bool ok = false;
    do {
        if(!flipper_format_file_open_always(ff, KEYFILE_PATH)) break;
        if(!flipper_format_write_header_cstr(ff, KEYFILE_TYPE, KEYFILE_VERSION)) break;
        if(!flipper_format_write_comment_cstr(
               ff, "Encrypted with this Flipper's own key; useless on any other device."))
            break;
        if(!flipper_format_write_string_cstr(ff, "Peer", name)) break;
        if(!flipper_format_write_hex(ff, "Key ID", key_id, sizeof(key_id))) break;
        if(!flipper_format_write_hex(ff, "IV", iv, sizeof(iv))) break;
        if(!flipper_format_write_hex(ff, "Key", enc, sizeof(enc))) break;
        ok = true;
    } while(false);
    flipper_format_free(ff);
    if(!ok) storage_simply_remove(storage, KEYFILE_PATH); // never leave half a key file
    furi_record_close(RECORD_STORAGE);
    bl_secure_zero(enc, sizeof(enc));
    return ok;
}

bool bl_keyfile_forget(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const bool ok = storage_simply_remove(storage, KEYFILE_PATH);
    furi_record_close(RECORD_STORAGE);
    return ok;
}
