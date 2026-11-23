/*
 * BruceLink shared core — remote-operation safety gate.
 * SPDX-License-Identifier: MIT
 */
#include "bl_safety.h"

#include <string.h>

void bl_safety_init(bl_safety_t* g) {
    if(g == NULL) return;
    memset(g, 0, sizeof(*g));
    g->window_ms = 1000; /* 1s window */
    g->max_frames_per_window = 64; /* conservative default */
    g->max_op_duration_ms = 30000; /* 30s max single active op */
}

void bl_safety_set_limits(
    bl_safety_t* g,
    uint32_t window_ms,
    uint32_t max_frames_per_window,
    uint32_t max_op_duration_ms) {
    if(g == NULL) return;
    if(window_ms) g->window_ms = window_ms;
    if(max_frames_per_window) g->max_frames_per_window = max_frames_per_window;
    if(max_op_duration_ms) g->max_op_duration_ms = max_op_duration_ms;
}

int bl_safety_enable_active(bl_safety_t* g, bool enable) {
    if(g == NULL) return BL_E_ARG;
    if(enable && g->estopped) return BL_E_DENIED;
    /* Deliberately does NOT touch the rate window: it records frames
     * actually transmitted and only elapsed time may expire it. Clearing it
     * here let a peer re-send "enable" before every op to reset the budget. */
    g->active_enabled = enable;
    return BL_OK;
}

int bl_safety_set_allowlist(
    bl_safety_t* g,
    const uint8_t (*bssids)[BL_BSSID_LEN],
    size_t count) {
    if(g == NULL) return BL_E_ARG;
    if(count > BL_ALLOWLIST_MAX) return BL_E_TOO_LARGE;
    for(size_t i = 0; i < count; i++) memcpy(g->allowlist[i], bssids[i], BL_BSSID_LEN);
    g->allowlist_count = (uint8_t)count;
    return BL_OK;
}

void bl_safety_estop(bl_safety_t* g) {
    if(g == NULL) return;
    g->estopped = true;
    g->active_enabled = false;
}

void bl_safety_reset(bl_safety_t* g) {
    if(g == NULL) return;
    g->estopped = false;
    g->active_enabled = false;
    /* The rate window survives resets (incl. a new session): it is device
     * transmit history, not session state, so reconnecting cannot refill it. */
}

void bl_safety_on_link_loss(bl_safety_t* g) {
    /* Fail safe: a dropped control link must halt any active test. */
    bl_safety_estop(g);
}

bool bl_safety_bssid_is_specific(const uint8_t bssid[BL_BSSID_LEN]) {
    if(bssid == NULL) return false;
    if(bssid[0] & 0x01u) return false; /* I/G bit: broadcast or multicast */
    uint8_t any = 0;
    for(size_t i = 0; i < BL_BSSID_LEN; i++) any |= bssid[i];
    return any != 0; /* 00:00:00:00:00:00 is not a station */
}

static void roll_window(bl_safety_t* g, uint32_t now_ms) {
    if(g->window_start_ms == 0 || now_ms - g->window_start_ms >= g->window_ms) {
        g->window_start_ms = now_ms ? now_ms : 1;
        g->frames_in_window = 0;
    }
}

uint32_t bl_safety_budget(bl_safety_t* g, uint32_t now_ms) {
    if(g == NULL) return 0;
    roll_window(g, now_ms);
    if(g->frames_in_window >= g->max_frames_per_window) return 0;
    return g->max_frames_per_window - g->frames_in_window;
}

void bl_safety_refund(bl_safety_t* g, uint32_t frames) {
    if(g == NULL) return;
    g->frames_in_window = (frames >= g->frames_in_window) ? 0 : g->frames_in_window - frames;
}

static bool target_in_scope(const bl_safety_t* g, const uint8_t target[BL_BSSID_LEN]) {
    if(g->allowlist_count == 0) return true; /* no scoping configured */
    for(uint8_t i = 0; i < g->allowlist_count; i++) {
        if(memcmp(g->allowlist[i], target, BL_BSSID_LEN) == 0) return true;
    }
    return false;
}

int bl_safety_authorize(
    bl_safety_t* g,
    bl_op_class_t klass,
    const uint8_t target[BL_BSSID_LEN],
    bool require_target,
    uint32_t frames,
    uint32_t now_ms) {
    if(g == NULL) return BL_E_ARG;
    if(g->estopped) return BL_E_DENIED;
    if(klass == BL_OP_PASSIVE) return BL_OK;

    /* active path */
    if(!g->active_enabled) return BL_E_DENIED;
    if(require_target) {
        /* never broadcast/multicast: the target must be one specific station */
        if(!bl_safety_bssid_is_specific(target)) return BL_E_DENIED;
        if(!target_in_scope(g, target)) return BL_E_DENIED;
    } else if(target != NULL && !target_in_scope(g, target)) {
        return BL_E_DENIED;
    }
    /* fixed-window rate limit */
    roll_window(g, now_ms);
    if(frames == 0) frames = 1;
    if(g->frames_in_window + frames > g->max_frames_per_window) return BL_E_LOCKED;
    g->frames_in_window += frames;
    return BL_OK;
}
