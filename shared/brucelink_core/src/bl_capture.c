/*
 * BruceLink shared core — capture session manager.
 * SPDX-License-Identifier: MIT
 */
#include "bl_capture.h"

#include <string.h>

#define BL_CAP_CH_MIN 1u
#define BL_CAP_CH_MAX 14u

void bl_capture_init(bl_capture_mgr_t* m, const bl_capture_backend_t* be, void* be_ctx) {
    if(m == NULL) return;
    memset(m, 0, sizeof(*m));
    m->be = be;
    m->be_ctx = be_ctx;
    m->state = BL_CAPST_IDLE;
}

int bl_capture_start(
    bl_capture_mgr_t* m,
    bl_capture_mode_t mode,
    uint8_t channel,
    uint32_t session_id,
    uint32_t now_ms) {
    if(m == NULL || m->be == NULL) return BL_E_ARG;
    if(m->state == BL_CAPST_RUNNING || m->state == BL_CAPST_STOPPING) return BL_E_BUSY;
    if(channel < BL_CAP_CH_MIN || channel > BL_CAP_CH_MAX) return BL_E_ARG;
    if(m->be->mode_available) {
        if(m->be->mode_available(m->be_ctx, mode) != 1) return BL_E_DENIED;
    }
    memset(&m->counts, 0, sizeof(m->counts));
    int rc = m->be->start(m->be_ctx, mode, channel);
    if(rc != BL_OK) {
        m->state = BL_CAPST_ERROR;
        m->last_error = rc;
        return rc;
    }
    m->mode = mode;
    m->channel = channel;
    m->session_id = session_id;
    m->start_ms = now_ms;
    m->last_error = BL_OK;
    m->state = BL_CAPST_RUNNING;
    return BL_OK;
}

int bl_capture_set_channel(bl_capture_mgr_t* m, uint8_t channel) {
    if(m == NULL || m->be == NULL) return BL_E_ARG;
    if(m->state != BL_CAPST_RUNNING) return BL_E_STATE;
    if(channel < BL_CAP_CH_MIN || channel > BL_CAP_CH_MAX) return BL_E_ARG;
    if(!m->be->set_channel) return BL_E_ARG;
    int rc = m->be->set_channel(m->be_ctx, channel);
    if(rc != BL_OK) {
        m->last_error = rc;
        return rc;
    }
    m->channel = channel;
    return BL_OK;
}

int bl_capture_poll(bl_capture_mgr_t* m) {
    if(m == NULL || m->be == NULL) return BL_E_ARG;
    if(m->state != BL_CAPST_RUNNING) return BL_E_STATE;
    if(!m->be->poll) return BL_E_ARG;
    bl_capture_counts_t c;
    memset(&c, 0, sizeof(c));
    int rc = m->be->poll(m->be_ctx, &c);
    if(rc != BL_OK) {
        /* a backend error during a running capture (e.g. storage full)
         * transitions to ERROR so the operator sees it; caller should stop */
        m->state = BL_CAPST_ERROR;
        m->last_error = rc;
        return rc;
    }
    m->counts = c;
    return BL_OK;
}

int bl_capture_stop(bl_capture_mgr_t* m) {
    if(m == NULL || m->be == NULL) return BL_E_ARG;
    if(m->state == BL_CAPST_IDLE) return BL_OK; /* idempotent */
    m->state = BL_CAPST_STOPPING;
    int rc = m->be->stop ? m->be->stop(m->be_ctx) : BL_OK;
    m->state = BL_CAPST_IDLE;
    if(rc != BL_OK) m->last_error = rc;
    return rc;
}
