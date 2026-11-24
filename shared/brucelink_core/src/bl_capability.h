/*
 * BruceLink shared core — capability registry.
 *
 * A device advertises which Bruce functions it actually exposes, their
 * class (passive vs active-RF), and constraints (needs authorization,
 * needs external hardware, not yet hardware-verified). The Flipper
 * queries this with BL_MSG_CAPS_REQ and renders menus from the reply,
 * so the UI never offers a function the connected firmware cannot run.
 *
 * Canonical capability ids are stable across versions.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_safety.h" /* bl_op_class_t */
#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical capability ids (stable). */
typedef enum {
    BL_CAP_DEVICE_INFO = 0x0001,
    BL_CAP_STATUS = 0x0002,
    BL_CAP_WIFI_SCAN = 0x0010,
    BL_CAP_BLE_SCAN = 0x0011,
    BL_CAP_SNIFF_HANDSHAKE = 0x0020, /* passive EAPOL 4-way capture */
    BL_CAP_SNIFF_RAW = 0x0021, /* full raw pcap (needs SD on StickS3) */
    BL_CAP_PCAP_LIST = 0x0022,
    BL_CAP_PCAP_XFER = 0x0023,
    BL_CAP_PCAP_DELETE = 0x0024,
    BL_CAP_DEAUTH = 0x0040, /* active: forces handshakes / DoS test */
    BL_CAP_BEACON_SPAM = 0x0041, /* active */
    BL_CAP_PROBE_FLOOD = 0x0042, /* active */
} bl_cap_id_t;

/* Capability flags. Authorization itself is the authenticated session,
 * so there is no per-capability "requires auth" flag. ACTIVE_RF marks a
 * capability that transmits and is therefore subject to the remote
 * safeguards (e-stop, dead-man, optional allowlist/rate limit). */
#define BL_CAP_F_ACTIVE_RF 0x01u /* transmits RF; honours the safety guard */
#define BL_CAP_F_REQUIRES_EXT_HW 0x02u /* needs hardware the StickS3 lacks */
#define BL_CAP_F_HW_UNVERIFIED 0x04u /* implemented but not verified on device */
#define BL_CAP_F_REQUIRES_TARGET 0x08u /* active op needs a BSSID target */
#define BL_CAP_F_JOB 0x10u /* runs as a job between restarts (JOB_START, ADR 0006) */

typedef struct {
    uint16_t id;
    const char* name; /* short stable slug, e.g. "wifi.sniff.handshake" */
    uint8_t op_class; /* bl_op_class_t */
    uint8_t flags; /* BL_CAP_F_* */
    uint8_t min_proto;
} bl_capability_desc_t;

typedef struct {
    const bl_capability_desc_t* items;
    size_t count;
} bl_capability_registry_t;

/* Look up a capability by id; NULL if absent. */
const bl_capability_desc_t* bl_caps_find(const bl_capability_registry_t* reg, uint16_t id);

/*
 * Serialize the registry into a CAPS payload (<= BL_MAX_PAYLOAD):
 *   u8 count, then per item: u16 id(LE), u8 op_class, u8 flags,
 *   u8 min_proto, u8 name_len, name[name_len].
 * Returns bytes written or BL_E_OVERFLOW.
 */
int bl_caps_serialize(const bl_capability_registry_t* reg, uint8_t* out, size_t cap);

/* Count parsed entries without materialising; validates structure.
 * Returns item count or negative bl_result_t. */
int bl_caps_validate(const uint8_t* buf, size_t len);

#ifdef __cplusplus
}
#endif
