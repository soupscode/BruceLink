/*
 * BruceLink shared core — self-contained cryptographic primitives.
 *
 * SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104), HKDF-SHA256 (RFC 5869)
 * and X25519 (RFC 7748). Implemented here so the Flipper app and the
 * ESP32 bridge run byte-identical code that is verified on the host
 * against published test vectors (tests/c/test_crypto.c).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_SHA256_LEN 32u
#define BL_SHA256_BLOCK 64u

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[BL_SHA256_BLOCK];
    size_t buflen;
} bl_sha256_ctx;

void bl_sha256_init(bl_sha256_ctx* c);
void bl_sha256_update(bl_sha256_ctx* c, const void* data, size_t len);
void bl_sha256_final(bl_sha256_ctx* c, uint8_t out[BL_SHA256_LEN]);
void bl_sha256(const void* data, size_t len, uint8_t out[BL_SHA256_LEN]);

typedef struct {
    bl_sha256_ctx inner;
    uint8_t okey[BL_SHA256_BLOCK];
} bl_hmac_ctx;

void bl_hmac_init(bl_hmac_ctx* c, const uint8_t* key, size_t key_len);
void bl_hmac_update(bl_hmac_ctx* c, const void* data, size_t len);
void bl_hmac_final(bl_hmac_ctx* c, uint8_t out[BL_SHA256_LEN]);
void bl_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const void* data,
    size_t len,
    uint8_t out[BL_SHA256_LEN]);

/* HKDF-SHA256. out_len <= 255*32. Returns BL_OK or BL_E_ARG. */
void bl_hkdf_extract(
    const uint8_t* salt,
    size_t salt_len,
    const uint8_t* ikm,
    size_t ikm_len,
    uint8_t prk[BL_SHA256_LEN]);
int bl_hkdf_expand(
    const uint8_t prk[BL_SHA256_LEN],
    const uint8_t* info,
    size_t info_len,
    uint8_t* out,
    size_t out_len);

/* X25519. Returns BL_OK, or BL_E_CRYPTO if the result is all-zero
 * (peer sent a low-order point). */
int bl_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
int bl_x25519_base(uint8_t pub[32], const uint8_t scalar[32]);

/* Constant-time equality; returns true if equal. */
bool bl_ct_equal(const void* a, const void* b, size_t len);
/* Zeroise memory in a way the optimiser will not elide. */
void bl_secure_zero(void* p, size_t len);

#ifdef __cplusplus
}
#endif
