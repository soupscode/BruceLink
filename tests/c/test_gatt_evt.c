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

static void t_gatt_offset_write_dropped(void) {
    uint8_t b[64];
    bl_gatt_evt_t e;
    const uint8_t d[4] = {1, 2, 3, 4};
    size_t n = mk_attr(b, RX_H + 1, 5, d, 4); /* partial write at offset */
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e)); /* consumed... */
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE); /* ...but no data delivered */
    n = mk_attr(b, RX_H + 1, 0x8000, d, 4); /* "more data" fragment flag */
    CHECK(bl_gatt_evt_parse(b, n, RX_H, TX_H, &e));
    CHECK_EQ_INT(e.kind, BL_GATT_EVT_NONE);
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
    RUN(t_gatt_foreign_events_ignored);
    RUN(t_gatt_offset_write_dropped);
    RUN(t_gatt_malformed_rejected);
    RUN(t_gatt_fuzz_small);
}
