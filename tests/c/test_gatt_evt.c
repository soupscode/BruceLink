/* Flipper BLE controller-event parser: well-formed, foreign, truncated and
 * lying packets. SPDX-License-Identifier: MIT */
#include "../../flipper/brucelink/transport/bl_gatt_evt.h"
#include "bl_test.h"

#define RX_H 0x0010u /* RX characteristic declaration handle; value = +1 */
#define TX_H 0x0012u /* TX characteristic declaration handle; CCCD  = +2 */

/* Build an attribute-modified vendor event. Returns total packet length. */
static size_t mk_attr(uint8_t* b, uint16_t attr, uint16_t offset, const uint8_t* d, uint16_t dlen) {
    size_t i = 0;
    b[i++] = 0x04; /* HCI event packet */
    b[i++] = 0xFF; /* vendor specific */
    b[i++] = (uint8_t)(2 + 8 + dlen); /* plen */
    b[i++] = 0x01;
    b[i++] = 0x0C; /* ecode 0x0C01 */
    b[i++] = 0x01;
    b[i++] = 0x08; /* connection handle 0x0801 */
    b[i++] = (uint8_t)attr;
    b[i++] = (uint8_t)(attr >> 8);
    b[i++] = (uint8_t)offset;
    b[i++] = (uint8_t)(offset >> 8);
    b[i++] = (uint8_t)dlen;
    b[i++] = (uint8_t)(dlen >> 8);
    for(uint16_t k = 0; k < dlen; k++) b[i++] = d[k];
    return i;
}

static void t_gatt_rx_data(void) {
    uint8_t b[300], d[244];
    for(int i = 0; i < 244; i++) d[i] = (uint8_t)(i ^ 0x5a);
    size_t n = mk_attr(b, RX_H + 1, 0, d, 244); /* largest chunk */
    bl_gatt_evt_t e;
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_RX_DATA);
    CHECK_EQ_INT(e.len, 244);
    CHECK(e.data == b + 13 && memcmp(e.data, d, 244) == 0);
    /* device-style call: only the packet's own length is known */
    CHECK(bl_gatt_evt_parse(b, 3u + b[2], RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_RX_DATA);
    /* zero-length write is still an RX event */
    n = mk_attr(b, RX_H + 1, 0, d, 0);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_RX_DATA);
    CHECK_EQ_INT(e.len, 0);
}

static void t_gatt_subscribe(void) {
    uint8_t b[32];
    bl_gatt_evt_t e;
    const uint8_t on[2] = {0x01, 0x00}, off[2] = {0x00, 0x00}, ind[2] = {0x02, 0x00};
    size_t n = mk_attr(b, TX_H + 2, 0, on, 2);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_SUBSCRIBED);
    n = mk_attr(b, TX_H + 2, 0, off, 2);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_UNSUBSCRIBED);
    /* indications only (bit 1) is not a notification subscription */
    n = mk_attr(b, TX_H + 2, 0, ind, 2);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_UNSUBSCRIBED);
    /* empty CCCD write: consumed, no state change reported */
    n = mk_attr(b, TX_H + 2, 0, on, 0);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
}

/* Review P2: a central that connects and never subscribes used to hold the
 * Flipper's only link forever (the radio stops advertising while connected).
 * The parser now reports the connection and a guard times it out. */
static void t_gatt_connection_complete(void) {
    bl_gatt_evt_t e;
    /* LE meta, subevent 0x01: status 0, handle 0x0801, role 1, peer..., params */
    uint8_t ok[] = {0x04, 0x3E, 0x13, 0x01, 0x00, 0x01, 0x08, 0x01, 0x00, 1, 2, 3, 4, 5, 6,
                    0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00};
    CHECK(!bl_gatt_evt_parse(ok, sizeof(ok), RX_H, TX_H, &e)); /* GAP needs it too */
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_CONNECTED);
    uint8_t failed[sizeof(ok)];
    memcpy(failed, ok, sizeof(ok));
    failed[4] = 0x3E; /* status: connection failed to be established */
    CHECK(!bl_gatt_evt_parse(failed, sizeof(failed), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    uint8_t other[sizeof(ok)];
    memcpy(other, ok, sizeof(ok));
    other[3] = 0x03; /* connection update complete: not a new link */
    CHECK(!bl_gatt_evt_parse(other, sizeof(other), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    uint8_t cut[] = {0x04, 0x3E, 0x02, 0x01, 0x00}; /* too short for a handle */
    CHECK(!bl_gatt_evt_parse(cut, sizeof(cut), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
}

static void t_sub_guard(void) {
    bl_sub_guard_t g;
    bl_sub_guard_init(&g);
    const uint32_t T = 5000;
    /* idle or subscribed: never fires */
    CHECK(!bl_sub_guard_expired(&g, false, false, 0, T));
    CHECK(!bl_sub_guard_expired(&g, false, false, 100000, T));
    CHECK(!bl_sub_guard_expired(&g, true, true, 100000, T));
    CHECK(!bl_sub_guard_expired(&g, true, true, 900000, T));
    /* connected, not subscribed: fires once, T after it was first seen */
    CHECK(!bl_sub_guard_expired(&g, true, false, 1000000, T));
    CHECK(!bl_sub_guard_expired(&g, true, false, 1000000 + T - 1, T));
    CHECK(bl_sub_guard_expired(&g, true, false, 1000000 + T, T));
    CHECK(!bl_sub_guard_expired(&g, true, false, 1000000 + T + 1, T)); /* re-armed, not repeated */
    CHECK(bl_sub_guard_expired(&g, true, false, 1000000 + 2 * T + 1, T));
    /* subscribing in time disarms it; a later unsubscribe starts a new wait */
    bl_sub_guard_init(&g);
    CHECK(!bl_sub_guard_expired(&g, true, false, 10, T));
    CHECK(!bl_sub_guard_expired(&g, true, true, 4000, T));
    CHECK(!bl_sub_guard_expired(&g, true, false, 6000, T)); /* wait restarts here */
    CHECK(!bl_sub_guard_expired(&g, true, false, 10999, T));
    CHECK(bl_sub_guard_expired(&g, true, false, 11000, T));
    /* a disconnect disarms it */
    bl_sub_guard_init(&g);
    CHECK(!bl_sub_guard_expired(&g, true, false, 10, T));
    CHECK(!bl_sub_guard_expired(&g, false, false, 20, T));
    CHECK(!bl_sub_guard_expired(&g, true, false, 9000, T));
    /* tick wraparound */
    bl_sub_guard_init(&g);
    CHECK(!bl_sub_guard_expired(&g, true, false, 0xFFFFF000u, T));
    CHECK(!bl_sub_guard_expired(&g, true, false, 0x00000100u, T)); /* +4352 ms */
    CHECK(bl_sub_guard_expired(&g, true, false, 0x00000400u, T)); /* +5120 ms */
}

static void t_gatt_mtu_and_disconnect_pass_through(void) {
    bl_gatt_evt_t e;
    /* ACI_ATT_EXCHANGE_MTU_RESP: conn 0x0801, mtu 247 */
    uint8_t mtu[] = {0x04, 0xFF, 0x06, 0x03, 0x0C, 0x01, 0x08, 0xF7, 0x00};
    CHECK(!bl_gatt_evt_parse(mtu, sizeof(mtu), RX_H, TX_H, &e)); /* not consumed */
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_MTU);
    CHECK_EQ_INT(e.mtu, 247);
    /* HCI disconnection complete: status 0, handle 0x0801, reason 0x13 */
    uint8_t dis[] = {0x04, 0x05, 0x04, 0x00, 0x01, 0x08, 0x13};
    CHECK(!bl_gatt_evt_parse(dis, sizeof(dis), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_DISCONNECTED);
}

static void t_gatt_foreign_events_ignored(void) {
    uint8_t b[64];
    bl_gatt_evt_t e;
    const uint8_t d[4] = {1, 2, 3, 4};
    /* another service's attribute */
    size_t n = mk_attr(b, 0x0030, 0, d, 4);
    CHECK(!bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    /* our TX value handle and RX declaration handle are not writable targets */
    n = mk_attr(b, TX_H + 1, 0, d, 4);
    CHECK(!bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    n = mk_attr(b, RX_H, 0, d, 4);
    CHECK(!bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    /* different vendor ecode, LE meta event, non-event packet type */
    uint8_t other[] = {0x04, 0xFF, 0x04, 0x16, 0x0C, 0x00, 0x00};
    CHECK(!bl_gatt_evt_parse(other, sizeof(other), RX_H, TX_H, &e));
    uint8_t meta[] = {0x04, 0x3E, 0x02, 0x01, 0x00};
    CHECK(!bl_gatt_evt_parse(meta, sizeof(meta), RX_H, TX_H, &e));
    uint8_t acl[] = {0x02, 0xFF, 0x02, 0x01, 0x0C};
    CHECK(!bl_gatt_evt_parse(acl, sizeof(acl), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
}

/* First StickS3 run: replies that fit one chunk worked, a larger reply
 * (Wi-Fi scan) never arrived, and nothing on the Flipper could say why.
 * The parser used to drop any RX event with a non-zero Offset field; ST
 * documents that field as "offset of this piece | 0x8000 if more follow",
 * i.e. the controller may split one write across events. The field now
 * travels with the data and bl_rx_asm_push joins the pieces. */
static void t_gatt_offset_field_reported(void) {
    uint8_t b[64];
    bl_gatt_evt_t e;
    const uint8_t d[4] = {1, 2, 3, 4};
    size_t n = mk_attr(b, RX_H + 1, 5, d, 4);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_RX_DATA);
    CHECK_EQ_INT(e.offset, 5);
    CHECK_EQ_INT(e.len, 4);
    n = mk_attr(b, RX_H + 1, 0x8000, d, 4);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_RX_DATA);
    CHECK_EQ_INT(e.offset, 0x8000);
    n = mk_attr(b, RX_H + 1, 0, d, 4);
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.offset, 0);
}

static void t_rx_asm_whole_write_passes_through(void) {
    bl_rx_asm_t a;
    bl_rx_asm_init(&a);
    uint8_t d[244];
    for(int i = 0; i < 244; i++) d[i] = (uint8_t)(i * 3);
    const uint8_t* c = NULL;
    uint16_t cl = 0;
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 244, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK(c == d); /* no copy */
    CHECK_EQ_INT(cl, 244);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 1, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK_EQ_INT(cl, 1);
}

static void t_rx_asm_joins_split_write(void) {
    bl_rx_asm_t a;
    bl_rx_asm_init(&a);
    uint8_t d[244];
    for(int i = 0; i < 244; i++) d[i] = (uint8_t)(i ^ 0x3c);
    const uint8_t* c = NULL;
    uint16_t cl = 0;
    /* 243 + 1: the largest chunk, one byte over what fits a single event
     * if the controller's limit is 243 */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 243, &c, &cl), BL_RX_ASM_PENDING);
    CHECK(c == NULL);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 243, d + 243, 1, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK_EQ_INT(cl, 244);
    CHECK(c != NULL && memcmp(c, d, 244) == 0);
    /* three pieces */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 100, &c, &cl), BL_RX_ASM_PENDING);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000 | 100, d + 100, 100, &c, &cl), BL_RX_ASM_PENDING);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 200, d + 200, 44, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK_EQ_INT(cl, 244);
    CHECK(c != NULL && memcmp(c, d, 244) == 0);
    /* the assembler is empty again: a whole write still passes through */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 10, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK(c == d);
}

static void t_rx_asm_rejects_what_does_not_fit_together(void) {
    bl_rx_asm_t a;
    bl_rx_asm_init(&a);
    uint8_t d[250];
    memset(d, 0x77, sizeof(d));
    const uint8_t* c = NULL;
    uint16_t cl = 0;
    /* a continuation with nothing in progress (a partial write at an offset) */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 5, d, 4, &c, &cl), BL_RX_ASM_REJECT);
    CHECK(c == NULL);
    /* a gap: second piece is not where the first ended */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 100, &c, &cl), BL_RX_ASM_PENDING);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 120, d, 20, &c, &cl), BL_RX_ASM_REJECT);
    /* ...and the write in progress is gone: its real next piece is refused too */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 100, d, 20, &c, &cl), BL_RX_ASM_REJECT);
    /* too long in total */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 200, &c, &cl), BL_RX_ASM_PENDING);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 200, d, 45, &c, &cl), BL_RX_ASM_REJECT);
    /* too long in one event */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 245, &c, &cl), BL_RX_ASM_REJECT);
    /* a new write abandons an unfinished one */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 50, &c, &cl), BL_RX_ASM_PENDING);
    d[0] = 0x11;
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 7, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK_EQ_INT(cl, 7);
    CHECK(c != NULL && c[0] == 0x11);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 50, d, 7, &c, &cl), BL_RX_ASM_REJECT);
    /* empty first piece, bad arguments */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0x8000, d, 0, &c, &cl), BL_RX_ASM_REJECT);
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, NULL, 3, &c, &cl), BL_RX_ASM_REJECT);
    CHECK_EQ_INT(bl_rx_asm_push(NULL, 0, d, 3, &c, &cl), BL_RX_ASM_REJECT);
    /* after all that, normal traffic still works */
    CHECK_EQ_INT(bl_rx_asm_push(&a, 0, d, 244, &c, &cl), BL_RX_ASM_CHUNK);
    CHECK_EQ_INT(cl, 244);
}

static void t_gatt_malformed_rejected(void) {
    uint8_t b[300], d[32];
    memset(d, 0xEE, sizeof(d));
    bl_gatt_evt_t e;
    size_t n = mk_attr(b, RX_H + 1, 0, d, 32);
    /* truncated at every length: never reads past `avail`, never accepts */
    for(size_t cut = 0; cut < n; cut++) {
        CHECK(!bl_gatt_evt_parse(b, cut, RX_H, TX_H, &e));
        CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    }
    /* data length field larger than the packet carries */
    n = mk_attr(b, RX_H + 1, 0, d, 8);
    b[11] = 200; /* Attr_Data_Length low byte */
    CHECK(!bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    /* plen too short for the fixed header */
    uint8_t shortv[] = {0x04, 0xFF, 0x05, 0x01, 0x0C, 0x01, 0x08, 0x11};
    CHECK(!bl_gatt_evt_parse(shortv, sizeof(shortv), RX_H, TX_H, &e));
    uint8_t shortmtu[] = {0x04, 0xFF, 0x04, 0x03, 0x0C, 0x01, 0x08};
    CHECK(!bl_gatt_evt_parse(shortmtu, sizeof(shortmtu), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    uint8_t shortdis[] = {0x04, 0x05, 0x02, 0x00, 0x01};
    CHECK(!bl_gatt_evt_parse(shortdis, sizeof(shortdis), RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
    /* null arguments */
    CHECK(!bl_gatt_evt_parse(NULL, 10, RX_H, TX_H, &e));
    CHECK(!bl_gatt_evt_parse(b, n, RX_H, TX_H, NULL));
}

/* Every byte pattern of a short packet: must not crash or over-read (ASan). */
static void t_gatt_fuzz_small(void) {
    uint8_t b[12];
    bl_gatt_evt_t e;
    uint32_t s = 0x1234567u;
    for(int iter = 0; iter < 20000; iter++) {
        for(size_t i = 0; i < sizeof(b); i++) {
            s = s * 1664525u + 1013904223u;
            b[i] = (uint8_t)(s >> 24);
        }
        b[0] = 0x04;
        b[1] = (iter & 1) ? 0xFF : b[1];
        size_t avail = (size_t)(s % (sizeof(b) + 1));
        bool used = bl_gatt_evt_parse(b, avail, RX_H, TX_H, &e);
        if(e.kind == BL_GATT_EVT_RX_DATA) {
            CHECK(used);
            CHECK(e.data >= b && e.data + e.len <= b + avail);
        }
    }
    CHECK(1);
}

void run_gatt_evt_tests(void) {
    RUN(t_gatt_rx_data);
    RUN(t_gatt_subscribe);
    RUN(t_gatt_mtu_and_disconnect_pass_through);
    RUN(t_gatt_connection_complete);
    RUN(t_sub_guard);
    RUN(t_gatt_foreign_events_ignored);
    RUN(t_gatt_offset_field_reported);
    RUN(t_rx_asm_whole_write_passes_through);
    RUN(t_rx_asm_joins_split_write);
    RUN(t_rx_asm_rejects_what_does_not_fit_together);
    RUN(t_gatt_malformed_rejected);
    RUN(t_gatt_fuzz_small);
}
