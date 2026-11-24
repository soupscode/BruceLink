/*
 * BruceLink vectors emitter — prints deterministic wire-format vectors
 * as JSON so an independent implementation (the Python reference) can
 * verify byte-for-byte interoperability of the frame codec, MAC, COBS
 * framing, PCAP headers and CRC32.
 * SPDX-License-Identifier: MIT
 */
#include "../shared/brucelink_core/src/bl_cobs.h"
#include "../shared/brucelink_core/src/bl_crypto.h"
#include "../shared/brucelink_core/src/bl_frame.h"
#include "../shared/brucelink_core/src/bl_pair.h"
#include "../shared/brucelink_core/src/bl_pcap.h"

#include <stdio.h>
#include <string.h>

static void emit_hex(const char* name, const uint8_t* b, size_t n) {
    printf("  \"%s\": \"", name);
    for(size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\"");
}

static void emit_frame(const char* name, const uint8_t* b, size_t n) {
    printf(",\n");
    emit_hex(name, b, n);
}

typedef struct {
    uint8_t next;
} counter_rng_t;
/* Deterministic "randomness" for vectors: 0x01, 0x02, ... per byte. */
static void counter_rng(void* ctx, uint8_t* buf, size_t len) {
    counter_rng_t* r = ctx;
    for(size_t i = 0; i < len; i++) buf[i] = r->next++;
}

/* A complete pairing run with fixed keys, so the Python reference can
 * recompute every derived value from ADR 0005 independently. */
static void emit_pairing(void) {
    counter_rng_t ri = {0x01}, rr = {0x81};
    bl_pair_t i, r;
    bl_pair_init(&i, BL_PAIR_INITIATOR, "Flipper Aster", counter_rng, &ri, 0);
    bl_pair_init(&r, BL_PAIR_RESPONDER, "StickS3", counter_rng, &rr, 0);
    uint8_t sk_i[32];
    memcpy(sk_i, i.sk, 32); /* the vector needs it; real code never copies it */
    uint8_t commit[BL_MAX_MSG], key[BL_MAX_MSG], reveal[BL_MAX_MSG], ci[BL_MAX_MSG],
        cr[BL_MAX_MSG];
    size_t keyl = 0, revl = 0, cil = 0, crl = 0, none = 0;
    uint8_t scratch[BL_MAX_MSG];
    int cn = bl_pair_start(&i, commit, sizeof(commit));
    bl_pair_rx(&r, commit, (size_t)cn, key, sizeof(key), &keyl);
    bl_pair_rx(&i, key, keyl, reveal, sizeof(reveal), &revl);
    bl_pair_rx(&r, reveal, revl, scratch, sizeof(scratch), &none);
    bl_pair_accept(&i, true, ci, sizeof(ci), &cil);
    bl_pair_accept(&r, true, cr, sizeof(cr), &crl);
    bl_pair_rx(&r, ci, cil, scratch, sizeof(scratch), &none);
    bl_pair_rx(&i, cr, crl, scratch, sizeof(scratch), &none);

    uint8_t shared[32];
    bl_x25519(shared, sk_i, r.pk);
    uint8_t psk[BL_KEY_LEN], kid[BL_PAIR_KEY_ID_LEN];
    bl_pair_result(&i, psk, kid);
    uint32_t code = 0;
    bl_pair_code(&i, &code);

    emit_frame("pair_commit", commit, (size_t)cn);
    emit_frame("pair_key", key, keyl);
    emit_frame("pair_reveal", reveal, revl);
    emit_frame("pair_confirm_i", ci, cil);
    emit_frame("pair_confirm_r", cr, crl);
    emit_frame("pair_shared", shared, sizeof(shared));
    emit_frame("pair_psk", psk, sizeof(psk));
    emit_frame("pair_key_id", kid, sizeof(kid));
    printf(",\n  \"pair_code\": %u,\n", (unsigned)code);
    printf("  \"pair_done\": %s", (bl_pair_state(&i) == BL_PS_DONE && bl_pair_state(&r) == BL_PS_DONE) ? "true" : "false");
}

int main(void) {
    printf("{\n");

    /* 1. MAC'd data frame with a fixed key, counter 7, payload 0..31 */
    uint8_t key[BL_KEY_LEN];
    for(unsigned i = 0; i < BL_KEY_LEN; i++) key[i] = (uint8_t)(0x40 + i);
    uint8_t pl[32];
    for(unsigned i = 0; i < 32; i++) pl[i] = (uint8_t)i;
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_STATUS, BL_FLAG_RESPONSE, 7, 32, pl};
    uint8_t frame[BL_MAX_MSG];
    int fn = bl_frame_encode(&f, key, sizeof(key), frame, sizeof(frame));
    emit_hex("frame_key", key, sizeof(key));
    printf(",\n");
    emit_hex("frame_mac", frame, (size_t)fn);
    printf(",\n");

    /* 2. COBS encoding of a payload containing zeros */
    uint8_t raw[] = {0x11, 0x00, 0x22, 0x00, 0x00, 0x33};
    uint8_t enc[64];
    int en = bl_cobs_encode(raw, sizeof(raw), enc, sizeof(enc));
    emit_hex("cobs_in", raw, sizeof(raw));
    printf(",\n");
    emit_hex("cobs_out", enc, (size_t)en);
    printf(",\n");

    /* 3. PCAP global + one record header */
    uint8_t gh[BL_PCAP_GLOBAL_LEN];
    bl_pcap_global_header(BL_PCAP_DEFAULT_SNAPLEN, BL_PCAP_LINKTYPE_IEEE802_11, gh, sizeof(gh));
    emit_hex("pcap_global", gh, sizeof(gh));
    printf(",\n");
    uint8_t rh[BL_PCAP_REC_LEN];
    bl_pcap_record_header(0x11223344, 0x000f4240, 100, 104, rh, sizeof(rh));
    emit_hex("pcap_record", rh, sizeof(rh));
    printf(",\n");

    /* 4. CRC32 of a known buffer */
    printf("  \"crc32_123456789\": \"%08x\"", bl_crc32("123456789", 9));

    /* 5. Pairing (ADR 0005) */
    emit_pairing();
    printf("\n");

    printf("}\n");
    return 0;
}
