/* Capability registry, PCAP writer, safety gate, capture lifecycle.
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_capability.h"
#include "../../shared/brucelink_core/src/bl_capture.h"
#include "../../shared/brucelink_core/src/bl_pcap.h"
#include "../../shared/brucelink_core/src/bl_safety.h"
#include "bl_test.h"

/* --------------------------------------------------------- registry */

static const bl_capability_desc_t g_caps[] = {
    {BL_CAP_DEVICE_INFO, "device.info", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_WIFI_SCAN, "wifi.scan", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_SNIFF_HANDSHAKE, "wifi.sniff.handshake", BL_OP_PASSIVE, BL_CAP_F_HW_UNVERIFIED, 1},
    {BL_CAP_SNIFF_RAW,
     "wifi.sniff.raw",
     BL_OP_PASSIVE,
     BL_CAP_F_REQUIRES_EXT_HW | BL_CAP_F_HW_UNVERIFIED,
     1},
    {BL_CAP_DEAUTH,
     "wifi.deauth",
     BL_OP_ACTIVE,
     BL_CAP_F_ACTIVE_RF | BL_CAP_F_REQUIRES_TARGET | BL_CAP_F_HW_UNVERIFIED,
     1},
};

static void t_registry_roundtrip(void) {
    bl_capability_registry_t reg = {g_caps, sizeof(g_caps) / sizeof(g_caps[0])};
    CHECK(bl_caps_find(&reg, BL_CAP_DEAUTH) != NULL);
    CHECK(bl_caps_find(&reg, 0x9999) == NULL);
    const bl_capability_desc_t* d = bl_caps_find(&reg, BL_CAP_DEAUTH);
    CHECK(d->op_class == BL_OP_ACTIVE);
    CHECK((d->flags & BL_CAP_F_ACTIVE_RF) != 0);

    uint8_t buf[BL_MAX_PAYLOAD];
    int n = bl_caps_serialize(&reg, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_EQ_INT(bl_caps_validate(buf, (size_t)n), (int)reg.count);
    /* truncation detected */
    CHECK(bl_caps_validate(buf, (size_t)n - 1) < 0);
    /* first byte is the count */
    CHECK_EQ_INT(buf[0], (int)reg.count);
}

/* ------------------------------------------------------------- pcap */

static void t_pcap_global_header_bytes(void) {
    uint8_t h[BL_PCAP_GLOBAL_LEN];
    int n = bl_pcap_global_header(
        BL_PCAP_DEFAULT_SNAPLEN, BL_PCAP_LINKTYPE_IEEE802_11, h, sizeof(h));
    CHECK_EQ_INT(n, 24);
    /* magic a1b2c3d4 little-endian */
    CHECK_EQ_INT(h[0], 0xd4);
    CHECK_EQ_INT(h[1], 0xc3);
    CHECK_EQ_INT(h[2], 0xb2);
    CHECK_EQ_INT(h[3], 0xa1);
    CHECK_EQ_INT(h[4], 2); /* v major LE */
    CHECK_EQ_INT(h[5], 0);
    CHECK_EQ_INT(h[6], 4); /* v minor */
    /* snaplen 2500 = 0x09C4 LE at offset 16 */
    CHECK_EQ_INT(h[16], 0xC4);
    CHECK_EQ_INT(h[17], 0x09);
    /* linktype 105 at offset 20 */
    CHECK_EQ_INT(h[20], 105);
    CHECK_EQ_INT(h[21], 0);
}

static void t_pcap_record_header_bytes(void) {
    uint8_t r[BL_PCAP_REC_LEN];
    int n = bl_pcap_record_header(0x11223344, 0x55667788, 100, 104, r, sizeof(r));
    CHECK_EQ_INT(n, 16);
    CHECK_EQ_INT(r[0], 0x44);
    CHECK_EQ_INT(r[3], 0x11);
    CHECK_EQ_INT(r[8], 100);
    CHECK_EQ_INT(r[12], 104);
}

static void t_crc32_known(void) {
    /* CRC-32/ISO-HDLC of "123456789" is 0xCBF43926 */
    CHECK_EQ_INT((long)bl_crc32("123456789", 9), (long)0xCBF43926u);
    /* streaming equals one-shot */
    uint32_t c = bl_crc32_init();
    c = bl_crc32_update(c, "12345", 5);
    c = bl_crc32_update(c, "6789", 4);
    CHECK_EQ_INT((long)bl_crc32_final(c), (long)0xCBF43926u);
}

/* ----------------------------------------------------------- safety */

static void t_safety_default_deny(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    uint8_t bssid[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    /* passive always ok */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_OK);
    /* active with nothing enabled -> denied (default-off) */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, bssid, true, 1, 1000), BL_E_DENIED);
}

static void t_safety_enable_and_scope(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    /* operator enables active ops over the (already authenticated) link */
    CHECK_EQ_INT(bl_safety_enable_active(&g, true), BL_OK);
    CHECK(bl_safety_active_ready(&g));

    uint8_t allowed[6] = {1, 2, 3, 4, 5, 6};
    uint8_t other[6] = {9, 9, 9, 9, 9, 9};
    /* with no scope, any specific target passes */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, other, true, 1, 1000), BL_OK);
    /* broadcast (no target) is never allowed for an active op */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, NULL, true, 1, 1000), BL_E_DENIED);

    /* operator confines the test to one target */
    uint8_t list[1][6];
    memcpy(list[0], allowed, 6);
    CHECK_EQ_INT(bl_safety_set_allowlist(&g, list, 1), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, allowed, true, 1, 1000), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, other, true, 1, 1000), BL_E_DENIED);

    /* disabling active ops denies again */
    bl_safety_enable_active(&g, false);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, allowed, true, 1, 1000), BL_E_DENIED);
}

static void t_safety_rate_limit(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_set_limits(&g, 1000, 10, 30000);
    bl_safety_enable_active(&g, true);
    uint8_t t[6] = {1, 1, 1, 1, 1, 1};
    /* 10 frames in the window OK, 11th blocked */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 10, 5000), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 5100), BL_E_LOCKED);
    /* next window resets budget */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 6200), BL_OK);
}

static void t_safety_estop_and_link_loss(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_enable_active(&g, true);
    bl_safety_estop(&g);
    CHECK(bl_safety_is_estopped(&g));
    CHECK(!bl_safety_active_ready(&g));
    /* even passive ops are refused while estopped (fail safe) */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_E_DENIED);
    /* re-enabling active is refused until reset */
    CHECK_EQ_INT(bl_safety_enable_active(&g, true), BL_E_DENIED);
    bl_safety_reset(&g);
    CHECK(!bl_safety_is_estopped(&g));
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_OK);

    /* link loss engages the e-stop automatically */
    bl_safety_enable_active(&g, true);
    bl_safety_on_link_loss(&g);
    CHECK(bl_safety_is_estopped(&g));
    CHECK(!bl_safety_active_ready(&g));
}

/* ---------------------------------------------------------- capture */

typedef struct {
    int start_rc, channel_rc, poll_rc, stop_rc;
    int started, stopped;
    int raw_available;
    uint32_t packets;
} mock_be_t;

static int mb_start(void* c, bl_capture_mode_t m, uint8_t ch) {
    (void)m;
    (void)ch;
    mock_be_t* b = c;
    if(b->start_rc != BL_OK) return b->start_rc;
    b->started = 1;
    return BL_OK;
}
static int mb_set_channel(void* c, uint8_t ch) {
    (void)ch;
    return ((mock_be_t*)c)->channel_rc;
}
static int mb_poll(void* c, bl_capture_counts_t* o) {
    mock_be_t* b = c;
    if(b->poll_rc != BL_OK) return b->poll_rc;
    b->packets += 5;
    o->packets = b->packets;
    o->eapol = 1;
    return BL_OK;
}
static int mb_stop(void* c) {
    mock_be_t* b = c;
    b->stopped = 1;
    return b->stop_rc;
}
static int mb_mode_avail(void* c, bl_capture_mode_t m) {
    mock_be_t* b = c;
    if(m == BL_CAP_MODE_RAW) return b->raw_available;
    return 1;
}
static const bl_capture_backend_t MOCK_BE = {
    mb_start, mb_set_channel, mb_poll, mb_stop, mb_mode_avail};

static void t_capture_lifecycle(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);

    /* invalid channel */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 99, 1, 100), BL_E_ARG);
    /* good start */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 1, 100), BL_OK);
    CHECK(bl_capture_is_running(&m));
    CHECK(be.started);
    /* double start -> busy */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 2, 100), BL_E_BUSY);
    /* poll accumulates counts */
    CHECK_EQ_INT(bl_capture_poll(&m), BL_OK);
    CHECK_EQ_INT(bl_capture_poll(&m), BL_OK);
    CHECK_EQ_INT(m.counts.packets, 10);
    /* channel change */
    CHECK_EQ_INT(bl_capture_set_channel(&m, 11), BL_OK);
    CHECK_EQ_INT(m.channel, 11);
    /* stop */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
    CHECK(be.stopped);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);
    /* stop again idempotent */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
}

static void t_capture_raw_mode_gated(void) {
    mock_be_t be = {0};
    be.raw_available = 0; /* StickS3 with no SD */
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_RAW, 1, 1, 0), BL_E_DENIED);
    CHECK(!be.started);
    /* handshake mode still available */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0), BL_OK);
}

static void t_capture_backend_start_failure(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    be.start_rc = BL_E_NO_RESOURCE;
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0), BL_E_NO_RESOURCE);
    CHECK_EQ_INT(m.state, BL_CAPST_ERROR);
    CHECK(!bl_capture_is_running(&m));
}

static void t_capture_poll_failure_transitions_error(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    be.poll_rc = BL_E_NO_RESOURCE; /* storage full mid-capture */
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0);
    CHECK_EQ_INT(bl_capture_poll(&m), BL_E_NO_RESOURCE);
    CHECK_EQ_INT(m.state, BL_CAPST_ERROR);
    /* operator can still stop to clean up */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
}

void run_capability_tests(void) {
    RUN(t_registry_roundtrip);
    RUN(t_pcap_global_header_bytes);
    RUN(t_pcap_record_header_bytes);
    RUN(t_crc32_known);
    RUN(t_safety_default_deny);
    RUN(t_safety_enable_and_scope);
    RUN(t_safety_rate_limit);
    RUN(t_safety_estop_and_link_loss);
    RUN(t_capture_lifecycle);
    RUN(t_capture_raw_mode_gated);
    RUN(t_capture_backend_start_failure);
    RUN(t_capture_poll_failure_transitions_error);
}
