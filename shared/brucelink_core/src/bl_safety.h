/*
 * BruceLink shared core — remote-operation safety gate.
 *
 * BruceLink is a remote frontend to Bruce's existing functions, not a
 * reimplementation. Authorization lives at the CONNECTION layer: the
 * session (bl_session) mutually authenticates the Flipper<->StickS3 link
 * with the provisioned pairing key. There is deliberately NO separate
 * authorization token per Bruce function.
 *
 * This gate adds only the safeguards that remote operation makes
 * necessary, because operating a device from across a wireless link
 * removes the physical stop (Bruce's only control over an active test
 * today is the device's own button). It enforces, all at the
 * connection/bridge layer:
 *   - a single session-level "active operations enabled" switch that the
 *     operator sets over the already-authenticated link (default OFF);
 *   - an emergency stop that latches the gate closed until reset, and is
 *     engaged automatically on link loss;
 *   - a transmission-rate / duration limiter so a wedged remote session
 *     cannot transmit unbounded. Its window records frames actually sent
 *     and is expired ONLY by elapsed time: enabling/disabling active ops,
 *     resetting the e-stop and starting a new session never clear it;
 *   - an OPTIONAL operator-configured target scope (allowlist). When
 *     empty, no scoping is applied; it is a convenience for confining a
 *     test, not an authorization credential.
 *
 * Passive operations (scans, info, capture RX, file ops) need none of
 * this; they pass whenever the gate is not emergency-stopped.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_ALLOWLIST_MAX 16u
#define BL_BSSID_LEN 6u

typedef enum {
    BL_OP_PASSIVE = 0, /* scans, info, capture RX, file ops */
    BL_OP_ACTIVE = 1, /* RF transmission (forwarded to Bruce) */
} bl_op_class_t;

typedef struct {
    bool active_enabled; /* operator enabled active ops over the authed link */
    bool estopped; /* latching emergency stop */
    uint8_t allowlist[BL_ALLOWLIST_MAX][BL_BSSID_LEN];
    uint8_t allowlist_count; /* 0 => no target scoping */
    /* transmission limiter */
    uint32_t window_ms;
    uint32_t max_frames_per_window;
    uint32_t window_start_ms;
    uint32_t frames_in_window;
    uint32_t max_op_duration_ms;
} bl_safety_t;

void bl_safety_init(bl_safety_t* g);

/* Configure the transmission limiter (0 leaves a field at its default). */
void bl_safety_set_limits(
    bl_safety_t* g,
    uint32_t window_ms,
    uint32_t max_frames_per_window,
    uint32_t max_op_duration_ms);

/*
 * Enable or disable active operations. The CALLER must only invoke this
 * in response to a command that arrived over an established, authenticated
 * session (bl_session_established) and reflects an explicit operator
 * opt-in. Enabling is refused while emergency-stopped.
 */
int bl_safety_enable_active(bl_safety_t* g, bool enable);

/* Replace the optional operator target scope. count 0 clears it (no
 * scoping). Returns BL_OK or BL_E_TOO_LARGE. */
int bl_safety_set_allowlist(
    bl_safety_t* g,
    const uint8_t (*bssids)[BL_BSSID_LEN],
    size_t count);

/* Emergency stop: latches closed and disables active ops. reset()
 * clears it (and leaves active ops disabled until re-enabled). reset()
 * keeps the rate window. */
void bl_safety_estop(bl_safety_t* g);
void bl_safety_reset(bl_safety_t* g);

/* Reliable connection-loss handling: engage e-stop and disable active
 * ops. The device calls this the instant the control link drops. */
void bl_safety_on_link_loss(bl_safety_t* g);

/*
 * True only for a specific (individual, unicast) station/AP address:
 * rejects all-zero and any group address (I/G bit set — this includes
 * the broadcast address ff:ff:ff:ff:ff:ff and all multicast).
 */
bool bl_safety_bssid_is_specific(const uint8_t bssid[BL_BSSID_LEN]);

/*
 * Frames still available in the current rate window at now_ms (rolls an
 * expired window first). Use it to bound how many frames an active op may
 * transmit, authorize exactly that many, then refund what was not used.
 */
uint32_t bl_safety_budget(bl_safety_t* g, uint32_t now_ms);

/* Return frames reserved by bl_safety_authorize but not transmitted. */
void bl_safety_refund(bl_safety_t* g, uint32_t frames);

/*
 * Authorize one operation at time now_ms. Passive ops pass unless
 * emergency-stopped. Active ops require: not e-stopped, active enabled,
 * a specific (unicast, non-zero) target when require_target is set —
 * never broadcast or multicast — the target within the scope when one is
 * configured, and spare budget in the rate window (reserving `frames`).
 * Returns BL_OK, BL_E_DENIED (estopped / disabled / non-specific or
 * missing target / target out of scope), or BL_E_LOCKED (rate limited).
 */
int bl_safety_authorize(
    bl_safety_t* g,
    bl_op_class_t klass,
    const uint8_t target[BL_BSSID_LEN],
    bool require_target,
    uint32_t frames,
    uint32_t now_ms);

static inline bool bl_safety_active_ready(const bl_safety_t* g) {
    return g && g->active_enabled && !g->estopped;
}
static inline bool bl_safety_is_estopped(const bl_safety_t* g) {
    return g && g->estopped;
}

#ifdef __cplusplus
}
#endif
