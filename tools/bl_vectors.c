/*
 * BruceLink vectors emitter — prints deterministic wire-format vectors
 * as JSON so an independent implementation (the Python reference) can
 * verify byte-for-byte interoperability of the frame codec, MAC, COBS
 * framing, PCAP headers and CRC32.
 * SPDX-License-Identifier: MIT
 */
#include "../shared/brucelink_core/src/bl_cobs.h"
#include "../shared/brucelink_core/src/bl_frame.h"
#include "../shared/brucelink_core/src/bl_pcap.h"

#include <stdio.h>
#include <string.h>

static void emit_hex(const char* name, const uint8_t* b, size_t n) {
    printf("  \"%s\": \"", name);
    for(size_t i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\"");
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
    printf("  \"crc32_123456789\": \"%08x\"\n", bl_crc32("123456789", 9));

    printf("}\n");
    return 0;
}
