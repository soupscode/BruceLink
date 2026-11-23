/*
 * BruceLink Flipper app — BLE controller event parser.
 * SPDX-License-Identifier: MIT
 */
#include "bl_gatt_evt.h"

#include <string.h>

#define HCI_EVENT_PKT_TYPE 0x04u
#define EVT_DISCONNECTION_COMPLETE 0x05u
#define EVT_LE_META 0x3Eu
#define SUBEVT_LE_CONNECTION_COMPLETE 0x01u
#define EVT_VENDOR 0xFFu
#define ECODE_ATTRIBUTE_MODIFIED 0x0C01u
#define ECODE_ATT_EXCHANGE_MTU_RESP 0x0C03u

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool bl_gatt_evt_parse(
    const uint8_t* pckt,
    size_t avail,
    uint16_t rx_char_handle,
    uint16_t tx_char_handle,
    bl_gatt_evt_t* out) {
    if(out == NULL) return false;
    out->kind = BL_GATT_EVT_NONE;
    out->data = NULL;
    out->len = 0;
    out->offset = 0;
    out->mtu = 0;
    if(pckt == NULL || avail < 3) return false;
    if(pckt[0] != HCI_EVENT_PKT_TYPE) return false;

    const uint8_t evt = pckt[1];
    const size_t plen = pckt[2];
    if(avail < 3 + plen) return false; /* truncated */
    const uint8_t* p = pckt + 3; /* event parameters, plen bytes */

    if(evt == EVT_DISCONNECTION_COMPLETE) {
        /* status(1) handle(2) reason(1) */
        if(plen < 4) return false;
        out->kind = BL_GATT_EVT_DISCONNECTED;
        return false; /* GAP must also process it */
    }
    if(evt == EVT_LE_META) {
        /* subevent(1) status(1) handle(2) ... ; status 0 = connected */
        if(plen >= 4 && p[0] == SUBEVT_LE_CONNECTION_COMPLETE && p[1] == 0x00u) {
            out->kind = BL_GATT_EVT_CONNECTED;
        }
        return false; /* GAP owns connection management */
    }
    if(evt != EVT_VENDOR || plen < 2) return false;

    const uint16_t ecode = rd16(p);
    const uint8_t* v = p + 2;
    const size_t vlen = plen - 2;

    if(ecode == ECODE_ATT_EXCHANGE_MTU_RESP) {
        if(vlen < 4) return false;
        out->kind = BL_GATT_EVT_MTU;
        out->mtu = rd16(v + 2);
        return false; /* GAP records it too */
    }
    if(ecode != ECODE_ATTRIBUTE_MODIFIED || vlen < 8) return false;

    const uint16_t attr = rd16(v + 2);
    const uint16_t offset = rd16(v + 4);
    const uint16_t dlen = rd16(v + 6);
    if((size_t)dlen > vlen - 8) return false; /* length field lies */
    const uint8_t* d = v + 8;

    if(attr == (uint16_t)(rx_char_handle + 1u)) {
        /* One write may span several events (see bl_rx_asm_push); the raw
         * Offset field travels with the data. */
        out->offset = offset;
        out->kind = BL_GATT_EVT_RX_DATA;
        out->data = d;
        out->len = dlen;
        return true;
    }
    if(attr == (uint16_t)(tx_char_handle + 2u)) {
        /* Client Characteristic Configuration: bit 0 = notifications */
        if(dlen < 1) return true;
        out->kind = (d[0] & 0x01u) ? BL_GATT_EVT_SUBSCRIBED : BL_GATT_EVT_UNSUBSCRIBED;
        return true;
    }
    return false; /* some other service's attribute */
}

void bl_sub_guard_init(bl_sub_guard_t* g) {
    if(g == NULL) return;
    g->waiting = false;
    g->since_ms = 0;
}

bool bl_sub_guard_expired(
    bl_sub_guard_t* g,
    bool connected,
    bool subscribed,
    uint32_t now_ms,
    uint32_t timeout_ms) {
    if(g == NULL) return false;
    if(!connected || subscribed) {
        g->waiting = false;
        return false;
    }
    if(!g->waiting) {
        g->waiting = true;
        g->since_ms = now_ms;
        return false;
    }
    if((uint32_t)(now_ms - g->since_ms) < timeout_ms) return false;
    g->waiting = false; /* fire once; a link that stays up is timed again */
    return true;
}

void bl_rx_asm_init(bl_rx_asm_t* a) {
    if(a != NULL) a->len = 0;
}

bl_rx_asm_result_t bl_rx_asm_push(
    bl_rx_asm_t* a,
    uint16_t offset_field,
    const uint8_t* data,
    uint16_t len,
    const uint8_t** chunk,
    uint16_t* chunk_len) {
    if(a == NULL || chunk == NULL || chunk_len == NULL || (data == NULL && len != 0)) {
        return BL_RX_ASM_REJECT;
    }
    *chunk = NULL;
    *chunk_len = 0;
    const uint16_t offset = (uint16_t)(offset_field & 0x7FFFu);
    const bool more = (offset_field & 0x8000u) != 0;

    if(offset == 0) {
        a->len = 0; /* a new write; an unfinished one is abandoned */
        if(!more) {
            if(len > BL_GATT_RX_CHUNK_MAX) return BL_RX_ASM_REJECT;
            *chunk = data; /* whole write in one event: no copy */
            *chunk_len = len;
            return BL_RX_ASM_CHUNK;
        }
    } else if(a->len == 0 || offset != a->len) {
        a->len = 0; /* not the next piece of anything we hold */
        return BL_RX_ASM_REJECT;
    }
    if((size_t)a->len + len > BL_GATT_RX_CHUNK_MAX) {
        a->len = 0;
        return BL_RX_ASM_REJECT;
    }
    if(len != 0) memcpy(a->buf + a->len, data, len);
    a->len = (uint16_t)(a->len + len);
    if(more) {
        /* A first piece with no data would be indistinguishable from "no
         * write in progress"; nothing sensible continues it. */
        if(a->len == 0) return BL_RX_ASM_REJECT;
        return BL_RX_ASM_PENDING;
    }
    *chunk = a->buf;
    *chunk_len = a->len;
    a->len = 0;
    return BL_RX_ASM_CHUNK;
}
