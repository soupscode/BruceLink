/*
 * BruceLink Flipper app — parser for the BLE controller events the custom
 * GATT service cares about.
 *
 * The Flipper's service event handler receives raw HCI event packets from
 * the STM32WB BLE stack. This module turns one packet into a small typed
 * result. It is portable C with no SDK dependency so it is host-tested
 * (tests/c/test_gatt_evt.c), including malformed and truncated packets;
 * the on-device handler stays a few lines.
 *
 * Packet layout and codes verified against STM32CubeWB v1.20.0
 * (stm32wb_copro @ 133182d, the release Momentum mntm-012 pins):
 *   hci_uart_pckt   { u8 type; data[] }                       tl.h
 *   hci_event_pckt  { u8 evt; u8 plen; data[] }               tl.h
 *   evt_blecore_aci { u16 ecode; data[] }                     tl.h
 *   HCI_DISCONNECTION_COMPLETE_EVT_CODE        0x05           ble_std.h
 *   HCI_LE_META_EVT_CODE                       0x3E           ble_std.h
 *   HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE     0x01           ble_std.h
 *   evt_le_meta_event { u8 subevent; data[] }                 tl.h
 *   hci_le_connection_complete_event_rp0 { u8 status; u16 conn; ... }
 *   HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE         0xFF           ble_std.h
 *   ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE     0x0C01         ble_vs_codes.h
 *   ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE       0x0C03         ble_vs_codes.h
 *   aci_gatt_attribute_modified_event_rp0
 *       { u16 conn; u16 attr_handle; u16 offset; u16 len; data[] }
 *   aci_att_exchange_mtu_resp_event_rp0 { u16 conn; u16 server_rx_mtu }
 * All multi-byte fields are little-endian.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BL_GATT_EVT_NONE = 0, /* not ours / malformed: let other handlers see it */
    BL_GATT_EVT_RX_DATA, /* central wrote the RX characteristic value */
    BL_GATT_EVT_SUBSCRIBED, /* central enabled notifications on TX */
    BL_GATT_EVT_UNSUBSCRIBED,
    BL_GATT_EVT_MTU, /* ATT MTU negotiated */
    BL_GATT_EVT_DISCONNECTED,
    BL_GATT_EVT_CONNECTED, /* a central connected (not yet subscribed) */
} bl_gatt_evt_kind_t;

typedef struct {
    bl_gatt_evt_kind_t kind;
    const uint8_t* data; /* RX_DATA: points into the packet */
    uint16_t len; /* RX_DATA: byte count */
    uint16_t offset; /* RX_DATA: the event's raw Offset field; feed it, with
                        data and len, to bl_rx_asm_push() */
    uint16_t mtu; /* MTU: negotiated ATT MTU */
} bl_gatt_evt_t;

/*
 * Parse one HCI event packet (starting at the hci_uart_pckt type byte).
 * `avail` is the number of readable bytes at `pckt`; pass 3 + pckt[2] when
 * only the packet's own length field is known. rx_char_handle and
 * tx_char_handle are the characteristic declaration handles returned when
 * the characteristics were added (value = handle + 1, CCCD = handle + 2).
 * Returns true and fills *out for an event this service consumes
 * (RX_DATA / SUBSCRIBED / UNSUBSCRIBED). MTU, CONNECTED and DISCONNECTED
 * also fill *out but return false: they must stay visible to the
 * firmware's own GAP handler.
 */
bool bl_gatt_evt_parse(
    const uint8_t* pckt,
    size_t avail,
    uint16_t rx_char_handle,
    uint16_t tx_char_handle,
    bl_gatt_evt_t* out);

/*
 * RX write assembler. One write from the central is one BruceLink chunk, but
 * the controller may report a write in more than one attribute-modified
 * event. Per ST's definition of the event's Offset field: bits 14-0 are the
 * offset of this piece within the write, and bit 15 set means more pieces
 * follow. Feed every RX_DATA event here; a chunk comes out when its last
 * piece arrives. A whole write in one event (offset field 0) is passed
 * through without copying.
 */
#define BL_GATT_RX_CHUNK_MAX 244u /* == BL_BLE_CHUNK_MAX (checked by the transport) */

typedef enum {
    BL_RX_ASM_PENDING = 0, /* piece stored; more to come */
    BL_RX_ASM_CHUNK, /* *chunk / *chunk_len hold a complete chunk */
    BL_RX_ASM_REJECT, /* piece does not continue the write in progress,
                         or the write is too long: everything discarded */
} bl_rx_asm_result_t;

typedef struct {
    uint8_t buf[BL_GATT_RX_CHUNK_MAX];
    uint16_t len; /* bytes of the write in progress; 0 = none */
} bl_rx_asm_t;

void bl_rx_asm_init(bl_rx_asm_t* a);
/* *chunk is valid until the next push (it points at `data` or at a->buf). */
bl_rx_asm_result_t bl_rx_asm_push(
    bl_rx_asm_t* a,
    uint16_t offset_field,
    const uint8_t* data,
    uint16_t len,
    const uint8_t** chunk,
    uint16_t* chunk_len);

/*
 * Subscription guard. While a central is connected the Flipper does not
 * advertise, so a central that connects and never enables notifications
 * would keep the StickS3 out indefinitely. Call on every poll with the
 * current link flags; returns true (once per timeout period) when a
 * connected central has gone timeout_ms without subscribing and should be
 * dropped. Wrap-safe.
 */
typedef struct {
    bool waiting;
    uint32_t since_ms;
} bl_sub_guard_t;

void bl_sub_guard_init(bl_sub_guard_t* g);
bool bl_sub_guard_expired(
    bl_sub_guard_t* g,
    bool connected,
    bool subscribed,
    uint32_t now_ms,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
