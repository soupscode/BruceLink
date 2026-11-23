/*
 * BruceLink shared core — cryptographic primitives.
 * The X25519 field arithmetic follows the structure of TweetNaCl
 * (public domain, Bernstein et al.), rewritten to avoid undefined
 * behaviour (no left shifts of negative values).
 * SPDX-License-Identifier: MIT
 */
#include "bl_crypto.h"

#include <string.h>

/* ---------------------------------------------------------------- SHA-256 */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(bl_sha256_ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for(int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
    }
    for(int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for(int i = 0; i < 64; i++) {
        uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
    c->state[4] += e;
    c->state[5] += f;
    c->state[6] += g;
    c->state[7] += h;
    bl_secure_zero(w, sizeof(w));
}

void bl_sha256_init(bl_sha256_ctx* c) {
    static const uint32_t iv[8] = {
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19};
    memcpy(c->state, iv, sizeof(iv));
    c->bitlen = 0;
    c->buflen = 0;
}

void bl_sha256_update(bl_sha256_ctx* c, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    while(len > 0) {
        size_t take = BL_SHA256_BLOCK - c->buflen;
        if(take > len) take = len;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        c->bitlen += (uint64_t)take * 8u;
        p += take;
        len -= take;
        if(c->buflen == BL_SHA256_BLOCK) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

void bl_sha256_final(bl_sha256_ctx* c, uint8_t out[BL_SHA256_LEN]) {
    uint64_t bitlen = c->bitlen;
    uint8_t pad = 0x80;
    bl_sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while(c->buflen != 56) bl_sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for(int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bitlen >> (56 - 8 * i));
    bl_sha256_update(c, lenb, 8);
    for(int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(c->state[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->state[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->state[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->state[i]);
    }
    bl_secure_zero(c, sizeof(*c));
}

void bl_sha256(const void* data, size_t len, uint8_t out[BL_SHA256_LEN]) {
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    bl_sha256_update(&c, data, len);
    bl_sha256_final(&c, out);
}

/* ------------------------------------------------------------ HMAC/HKDF */

void bl_hmac_init(bl_hmac_ctx* c, const uint8_t* key, size_t key_len) {
    uint8_t k[BL_SHA256_BLOCK];
    memset(k, 0, sizeof(k));
    if(key_len > BL_SHA256_BLOCK) {
        bl_sha256(key, key_len, k);
    } else if(key_len > 0) {
        memcpy(k, key, key_len);
    }
    uint8_t ipad[BL_SHA256_BLOCK];
    for(size_t i = 0; i < BL_SHA256_BLOCK; i++) {
        ipad[i] = k[i] ^ 0x36;
        c->okey[i] = k[i] ^ 0x5c;
    }
    bl_sha256_init(&c->inner);
    bl_sha256_update(&c->inner, ipad, sizeof(ipad));
    bl_secure_zero(k, sizeof(k));
    bl_secure_zero(ipad, sizeof(ipad));
}

void bl_hmac_update(bl_hmac_ctx* c, const void* data, size_t len) {
    bl_sha256_update(&c->inner, data, len);
}

void bl_hmac_final(bl_hmac_ctx* c, uint8_t out[BL_SHA256_LEN]) {
    uint8_t ih[BL_SHA256_LEN];
    bl_sha256_final(&c->inner, ih);
    bl_sha256_ctx o;
    bl_sha256_init(&o);
    bl_sha256_update(&o, c->okey, sizeof(c->okey));
    bl_sha256_update(&o, ih, sizeof(ih));
    bl_sha256_final(&o, out);
    bl_secure_zero(ih, sizeof(ih));
    bl_secure_zero(c, sizeof(*c));
}

void bl_hmac_sha256(
    const uint8_t* key,
    size_t key_len,
    const void* data,
    size_t len,
    uint8_t out[BL_SHA256_LEN]) {
    bl_hmac_ctx c;
    bl_hmac_init(&c, key, key_len);
    bl_hmac_update(&c, data, len);
    bl_hmac_final(&c, out);
}

void bl_hkdf_extract(
    const uint8_t* salt,
    size_t salt_len,
    const uint8_t* ikm,
    size_t ikm_len,
    uint8_t prk[BL_SHA256_LEN]) {
    static const uint8_t zeros[BL_SHA256_LEN] = {0};
    if(salt == NULL || salt_len == 0) {
        salt = zeros;
        salt_len = sizeof(zeros);
    }
    bl_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

int bl_hkdf_expand(
    const uint8_t prk[BL_SHA256_LEN],
    const uint8_t* info,
    size_t info_len,
    uint8_t* out,
    size_t out_len) {
    if(out_len > 255u * BL_SHA256_LEN || (info == NULL && info_len != 0)) return BL_E_ARG;
    uint8_t t[BL_SHA256_LEN];
    size_t tlen = 0;
    uint8_t counter = 1;
    size_t done = 0;
    while(done < out_len) {
        bl_hmac_ctx c;
        bl_hmac_init(&c, prk, BL_SHA256_LEN);
        bl_hmac_update(&c, t, tlen);
        if(info_len) bl_hmac_update(&c, info, info_len);
        bl_hmac_update(&c, &counter, 1);
        bl_hmac_final(&c, t);
        tlen = BL_SHA256_LEN;
        size_t take = out_len - done;
        if(take > BL_SHA256_LEN) take = BL_SHA256_LEN;
        memcpy(out + done, t, take);
        done += take;
        counter++;
    }
    bl_secure_zero(t, sizeof(t));
    return BL_OK;
}

/* ---------------------------------------------------------------- X25519 */

typedef int64_t gf[16];

static void car25519(gf o) {
    for(int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16; /* arithmetic shift (GCC/Clang) */
        if(i < 15) {
            o[i + 1] += c - 1;
        } else {
            o[0] += 38 * (c - 1);
        }
        o[i] -= c * 65536;
    }
}

static void sel25519(gf p, gf q, int64_t b) {
    int64_t c = ~(b - 1);
    for(int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t* o, const gf n) {
    gf m, t;
    for(int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for(int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for(int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for(int i = 0; i < 16; i++) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xff);
    }
}

static void unpack25519(gf o, const uint8_t* n) {
    for(int i = 0; i < 16; i++) o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void fadd(gf o, const gf a, const gf b) {
    for(int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fsub(gf o, const gf a, const gf b) {
    for(int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for(int i = 0; i < 31; i++) t[i] = 0;
    for(int i = 0; i < 16; i++)
        for(int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for(int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for(int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fsq(gf o, const gf a) {
    fmul(o, a, a);
}

static void finv(gf o, const gf in) {
    gf c;
    for(int a = 0; a < 16; a++) c[a] = in[a];
    for(int a = 253; a >= 0; a--) {
        fsq(c, c);
        if(a != 2 && a != 4) fmul(c, c, in);
    }
    for(int a = 0; a < 16; a++) o[a] = c[a];
}

int bl_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    static const gf k121665 = {0xDB41, 1};
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    for(int i = 0; i < 31; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((scalar[31] & 127) | 64);
    z[0] &= 248;
    unpack25519(x, point);
    for(int i = 0; i < 16; i++) {
        b[i] = x[i];
        d[i] = a[i] = c[i] = 0;
    }
    a[0] = d[0] = 1;
    for(int i = 254; i >= 0; --i) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fadd(e, a, c);
        fsub(a, a, c);
        fadd(c, b, d);
        fsub(b, b, d);
        fsq(d, e);
        fsq(f, a);
        fmul(a, c, a);
        fmul(c, b, e);
        fadd(e, a, c);
        fsub(a, a, c);
        fsq(b, a);
        fsub(c, d, f);
        fmul(a, c, k121665);
        fadd(a, a, d);
        fmul(c, c, a);
        fmul(a, d, f);
        fmul(d, b, x);
        fsq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    finv(c, c);
    fmul(a, a, c);
    pack25519(out, a);
    bl_secure_zero(z, sizeof(z));
    bl_secure_zero(a, sizeof(a));
    bl_secure_zero(b, sizeof(b));
    bl_secure_zero(c, sizeof(c));
    bl_secure_zero(d, sizeof(d));
    bl_secure_zero(e, sizeof(e));
    bl_secure_zero(f, sizeof(f));
    uint8_t acc = 0;
    for(int i = 0; i < 32; i++) acc |= out[i];
    return acc == 0 ? BL_E_CRYPTO : BL_OK;
}

int bl_x25519_base(uint8_t pub[32], const uint8_t scalar[32]) {
    static const uint8_t base[32] = {9};
    return bl_x25519(pub, scalar, base);
}

/* ----------------------------------------------------------------- utils */

bool bl_ct_equal(const void* a, const void* b, size_t len) {
    const volatile uint8_t* x = (const volatile uint8_t*)a;
    const volatile uint8_t* y = (const volatile uint8_t*)b;
    uint8_t d = 0;
    for(size_t i = 0; i < len; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}

void bl_secure_zero(void* p, size_t len) {
    volatile uint8_t* v = (volatile uint8_t*)p;
    while(len--) *v++ = 0;
}
