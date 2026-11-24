/* Crypto known-answer tests against published vectors. SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_crypto.h"
#include "bl_test.h"

static void t_sha256_nist(void) {
    uint8_t d[BL_SHA256_LEN];
    uint8_t exp[BL_SHA256_LEN];
    /* FIPS 180-4 "abc" */
    bl_sha256("abc", 3, d);
    bl_hex2bin("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", exp, 32);
    CHECK(memcmp(d, exp, 32) == 0);
    /* empty string */
    bl_sha256("", 0, d);
    bl_hex2bin("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", exp, 32);
    CHECK(memcmp(d, exp, 32) == 0);
    /* 448-bit message spanning a block boundary */
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    bl_sha256(m, strlen(m), d);
    bl_hex2bin("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", exp, 32);
    CHECK(memcmp(d, exp, 32) == 0);
}

static void t_sha256_streaming(void) {
    /* streaming in odd chunks must equal one-shot */
    const char* m = "the quick brown fox jumps over the lazy dog, several times over!!";
    uint8_t one[32], strm[32];
    bl_sha256(m, strlen(m), one);
    bl_sha256_ctx c;
    bl_sha256_init(&c);
    size_t len = strlen(m), off = 0;
    size_t steps[] = {1, 7, 13, 31, 3};
    int i = 0;
    while(off < len) {
        size_t take = steps[i++ % 5];
        if(take > len - off) take = len - off;
        bl_sha256_update(&c, m + off, take);
        off += take;
    }
    bl_sha256_final(&c, strm);
    CHECK(memcmp(one, strm, 32) == 0);
}

static void t_hmac_rfc4231(void) {
    uint8_t mac[BL_SHA256_LEN], exp[BL_SHA256_LEN];
    /* RFC 4231 test case 1 */
    uint8_t key[20];
    memset(key, 0x0b, sizeof(key));
    bl_hmac_sha256(key, sizeof(key), "Hi There", 8, mac);
    bl_hex2bin("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", exp, 32);
    CHECK(memcmp(mac, exp, 32) == 0);
    /* case 2: key "Jefe" */
    bl_hmac_sha256(
        (const uint8_t*)"Jefe", 4, "what do ya want for nothing?", 28, mac);
    bl_hex2bin("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", exp, 32);
    CHECK(memcmp(mac, exp, 32) == 0);
}

static void t_hkdf_rfc5869(void) {
    /* RFC 5869 appendix A.1 (SHA-256) */
    uint8_t ikm[22], salt[13], info[10], prk[32], okm[42], exp[42];
    memset(ikm, 0x0b, sizeof(ikm));
    for(int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for(int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    bl_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    uint8_t prk_exp[32];
    bl_hex2bin("077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", prk_exp, 32);
    CHECK(memcmp(prk, prk_exp, 32) == 0);
    CHECK_EQ_INT(bl_hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm)), BL_OK);
    bl_hex2bin(
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865",
        exp, 42);
    CHECK(memcmp(okm, exp, 42) == 0);
}

static void t_x25519_rfc7748(void) {
    uint8_t out[32], scalar[32], point[32], exp[32];
    /* RFC 7748 section 5.2 */
    bl_hex2bin("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    bl_hex2bin("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    CHECK_EQ_INT(bl_x25519(out, scalar, point), BL_OK);
    bl_hex2bin("c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", exp, 32);
    CHECK(memcmp(out, exp, 32) == 0);
    /* base-point: scalar*9 = public key, RFC 7748 Alice */
    bl_hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", exp, 32);
    uint8_t a_sk[32];
    bl_hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_sk, 32);
    /* Alice private -> Alice public */
    uint8_t a_priv[32];
    bl_hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv, 32);
    (void)a_sk;
    /* RFC 7748: Alice's private key -> public 8520f0...; use documented pair */
    uint8_t alice_priv[32], alice_pub_exp[32], alice_pub[32];
    bl_hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", alice_priv, 32);
    bl_hex2bin("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alice_pub_exp, 32);
    CHECK_EQ_INT(bl_x25519_base(alice_pub, alice_priv), BL_OK);
    CHECK(memcmp(alice_pub, alice_pub_exp, 32) == 0);
}

static void t_x25519_ecdh_agreement(void) {
    uint8_t a_sk[32], b_sk[32], a_pk[32], b_pk[32], s1[32], s2[32];
    bl_hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_sk, 32);
    bl_hex2bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_sk, 32);
    bl_x25519_base(a_pk, a_sk);
    bl_x25519_base(b_pk, b_sk);
    CHECK_EQ_INT(bl_x25519(s1, a_sk, b_pk), BL_OK);
    CHECK_EQ_INT(bl_x25519(s2, b_sk, a_pk), BL_OK);
    CHECK(memcmp(s1, s2, 32) == 0);
    /* RFC 7748 shared secret */
    uint8_t exp[32];
    bl_hex2bin("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", exp, 32);
    CHECK(memcmp(s1, exp, 32) == 0);
}

static void t_x25519_low_order(void) {
    /* all-zero output (low-order point) must be rejected */
    uint8_t out[32], scalar[32], point[32];
    memset(scalar, 0x11, 32);
    memset(point, 0, 32); /* order-1 point -> zero shared secret */
    CHECK_EQ_INT(bl_x25519(out, scalar, point), BL_E_CRYPTO);
}

static void t_ct_equal(void) {
    uint8_t a[16], b[16];
    memset(a, 0xAB, 16);
    memset(b, 0xAB, 16);
    CHECK(bl_ct_equal(a, b, 16));
    b[15] ^= 1;
    CHECK(!bl_ct_equal(a, b, 16));
}

/* Review 2026-11-23: keys longer than a block, multi-block HKDF output and
 * the other low-order points had no known-answer test. */
static void t_hmac_long_key_rfc4231(void) {
    uint8_t key[131], mac[BL_SHA256_LEN], exp[BL_SHA256_LEN];
    memset(key, 0xaa, sizeof(key));
    /* RFC 4231 test case 6: key longer than a block */
    static const char m6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    bl_hmac_sha256(key, sizeof(key), m6, sizeof(m6) - 1, mac);
    bl_hex2bin("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", exp, 32);
    CHECK(memcmp(mac, exp, 32) == 0);
    /* test case 7: key and data both longer than a block */
    static const char m7[] =
        "This is a test using a larger than block-size key and a larger than block-size data. "
        "The key needs to be hashed before being used by the HMAC algorithm.";
    bl_hmac_sha256(key, sizeof(key), m7, sizeof(m7) - 1, mac);
    bl_hex2bin("9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", exp, 32);
    CHECK(memcmp(mac, exp, 32) == 0);
}

static void t_hkdf_long_rfc5869(void) {
    /* RFC 5869 appendix A.2: 80-byte inputs, 82-byte (three-block) output */
    uint8_t ikm[80], salt[80], info[80], prk[32], okm[82], exp[82];
    for(int i = 0; i < 80; i++) {
        ikm[i] = (uint8_t)i;
        salt[i] = (uint8_t)(0x60 + i);
        info[i] = (uint8_t)(0xb0 + i);
    }
    bl_hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    bl_hex2bin("06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244", exp, 32);
    CHECK(memcmp(prk, exp, 32) == 0);
    CHECK_EQ_INT(bl_hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm)), BL_OK);
    bl_hex2bin(
        "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
        "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
        "cc30c58179ec3e87c14c01d5c1f3434f1d87",
        exp, 82);
    CHECK(memcmp(okm, exp, 82) == 0);
}

static void t_x25519_all_low_order_points(void) {
    /* The seven encodings of points of order 1, 2, 4 and 8 (u = 0, 1, the
     * two order-8 points, p-1, p, p+1): each gives an all-zero secret. */
    static const char* const pts[7] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    uint8_t out[32], scalar[32], point[32];
    bl_hex2bin("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    for(int i = 0; i < 7; i++) {
        bl_hex2bin(pts[i], point, 32);
        CHECK_EQ_INT(bl_x25519(out, scalar, point), BL_E_CRYPTO);
        /* the high bit is ignored (RFC 7748), so it is no way around it */
        point[31] |= 0x80;
        CHECK_EQ_INT(bl_x25519(out, scalar, point), BL_E_CRYPTO);
    }
}

/* RFC 5869 appendix A.3: zero-length salt and info. Pins the expand loop's
 * first block (T(0) is empty) and the no-info path, so the cppcheck fix in
 * bl_hkdf_expand (2026-11-24) provably keeps the output unchanged. */
static void t_hkdf_empty_salt_info_rfc5869(void) {
    uint8_t ikm[22], prk[32], okm[42], exp[42];
    memset(ikm, 0x0b, sizeof(ikm));
    bl_hkdf_extract(NULL, 0, ikm, sizeof(ikm), prk);
    bl_hex2bin("19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04", exp, 32);
    CHECK(memcmp(prk, exp, 32) == 0);
    CHECK_EQ_INT(bl_hkdf_expand(prk, NULL, 0, okm, sizeof(okm)), BL_OK);
    bl_hex2bin(
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
        "9d201395faa4b61a96c8",
        exp, 42);
    CHECK(memcmp(okm, exp, 42) == 0);
}

void run_crypto_tests(void) {
    RUN(t_hmac_long_key_rfc4231);
    RUN(t_hkdf_long_rfc5869);
    RUN(t_hkdf_empty_salt_info_rfc5869);
    RUN(t_x25519_all_low_order_points);
    RUN(t_sha256_nist);
    RUN(t_sha256_streaming);
    RUN(t_hmac_rfc4231);
    RUN(t_hkdf_rfc5869);
    RUN(t_x25519_rfc7748);
    RUN(t_x25519_ecdh_agreement);
    RUN(t_x25519_low_order);
    RUN(t_ct_equal);
}
