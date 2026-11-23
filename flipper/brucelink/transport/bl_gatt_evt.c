/*
 * BruceLink Flipper app — BLE controller event parser.
 * SPDX-License-Identifier: MIT
 */
#include "bl_gatt_evt.h"

#define HCI_EVENT_PKT_TYPE 0x04u
#define EVT_DISCONNECTION_COMPLETE 0x05u
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
        /* Chunks never exceed one event. A non-zero offset (bit 15 flags a
         * fragmented long write) is not something a BruceLink central
         * sends: consume the event and drop the data. */
        if(offset != 0) return true;
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
