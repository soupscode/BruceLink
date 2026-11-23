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
    m->backend_active = false;
}

/* Stop the backend if (and only if) it is still active. */
static int backend_stop_once(bl_capture_mgr_t* m) {
    int rc = BL_OK;
    if(m->backend_active && m->be != NULL && m->be->stop != NULL) {
        rc = m->be->stop(m->be_ctx);
    }
    m->backend_active = false;
    return rc;
}

int bl_capture_start(
    bl_capture_mgr_t* m,
    bl_capture_mode_t mode,
    uint8_t channel,
    uint32_t session_id,
    uint32_t now_ms) {
    if(m == NULL || m->be == NULL || m->be->start == NULL) return BL_E_ARG;
    if(m->state == BL_CAPST_RUNNING || m->state == BL_CAPST_STOPPING) return BL_E_BUSY;
    /* One unsigned bound check: enum signedness differs between targets
     * (unsigned short-enums on the Flipper toolchain), and a negative
     * value converts to a huge unsigned one, so this rejects both ends. */
    if((unsigned)mode > (unsigned)BL_CAP_MODE_DEAUTH_PCAP) return BL_E_ARG;
    if(channel < BL_CAP_CH_MIN || channel > BL_CAP_CH_MAX) return BL_E_ARG;
    if(m->be->mode_available) {
        if(m->be->mode_available(m->be_ctx, mode) != 1) return BL_E_DENIED;
    }
    memset(&m->counts, 0, sizeof(m->counts));
    int rc = m->be->start(m->be_ctx, mode, channel);
    if(rc != BL_OK) {
        /* contract: a failed start leaves the backend stopped */
        m->backend_active = false;
        m->state = BL_CAPST_ERROR;
        m->last_error = rc;
        return rc;
    }
    m->backend_active = true;
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
        /* A failure mid-capture (e.g. LittleFS full) must not leave the
         * radio in promiscuous mode or the pcap file open: stop and flush
         * the backend now, exactly once, then surface ERROR to the operator.
         * The last good counters are kept. */
        (void)backend_stop_once(m);
        m->state = BL_CAPST_ERROR;
        m->last_error = rc;
        return rc;
    }
    m->counts = c;
    return BL_OK;
}

int bl_capture_stop(bl_capture_mgr_t* m) {
    if(m == NULL) return BL_E_ARG;
    if(m->state == BL_CAPST_IDLE) return BL_OK; /* idempotent */
    m->state = BL_CAPST_STOPPING;
    int rc = backend_stop_once(m);
    m->state = BL_CAPST_IDLE;
    if(rc != BL_OK) m->last_error = rc;
    return rc;
}
