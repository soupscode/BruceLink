/*
 * BruceLink shared core — jobs (see bl_job.h, ADR 0006).
 * SPDX-License-Identifier: MIT
 */
#include "bl_job.h"

#include "bl_pcap.h" /* bl_crc32 */
#include "bl_proto.h"

#include <string.h>

static const char kInterrupted[] =
    "The StickS3 restarted during the job (crash, watchdog or power dip). It was not run again.";

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)(v >> 24);
}
static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | (uint16_t)(p[1] << 8));
}
static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- record --------------------------------------------------------- */

static uint32_t record_crc(const bl_job_record_t* r) {
    return bl_crc32(r, offsetof(bl_job_record_t, crc));
}

static void seal(bl_job_record_t* r) {
    r->crc = record_crc(r);
}

void bl_job_record_clear(bl_job_record_t* r) {
    if(r == NULL) return;
    memset(r, 0, sizeof(*r));
}

int bl_job_record_create(
    bl_job_record_t* r,
    uint32_t job_id,
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len) {
    if(r == NULL || job_id == 0 || max_s == 0 || max_s > BL_JOB_MAX_S || args_len > BL_JOB_ARGS_MAX ||
       (args_len > 0 && args == NULL)) {
        return BL_E_ARG;
    }
    memset(r, 0, sizeof(*r)); /* padding-free layout, but keep it deterministic */
    r->magic = BL_JOB_MAGIC;
    r->version = BL_JOB_VERSION;
    r->job_id = job_id;
    r->cap_id = cap_id;
    r->max_s = max_s;
    r->state = BL_JOB_S_PENDING;
    r->resume_link = 1; /* if the job's own boot never finishes it */
    r->args_len = (uint8_t)args_len;
    if(args_len > 0) memcpy(r->args, args, args_len);
    seal(r);
    return BL_OK;
}

bool bl_job_record_valid(const bl_job_record_t* r) {
    if(r == NULL) return false;
    if(r->magic != BL_JOB_MAGIC || r->version != BL_JOB_VERSION) return false;
    if(r->crc != record_crc(r)) return false;
    if(r->job_id == 0 || r->max_s == 0 || r->max_s > BL_JOB_MAX_S) return false;
    if(r->args_len > BL_JOB_ARGS_MAX || r->result_len > BL_JOB_RESULT_MAX) return false;
    if(r->state < BL_JOB_S_PENDING || r->state > BL_JOB_S_FINISHED) return false;
    if(r->resume_link > 1) return false;
    if(r->state == BL_JOB_S_FINISHED &&
       (r->outcome < BL_JOB_DONE || r->outcome > BL_JOB_INTERRUPTED)) {
        return false;
    }
    return true;
}

void bl_job_record_finish(bl_job_record_t* r, bl_job_outcome_t outcome, const char* text, size_t len) {
    if(r == NULL) return;
    if(text == NULL) len = 0;
    if(len > BL_JOB_RESULT_MAX) len = BL_JOB_RESULT_MAX;
    memset(r->result, 0, sizeof(r->result));
    if(len > 0) memcpy(r->result, text, len);
    r->result_len = (uint16_t)len;
    r->outcome = (uint8_t)outcome;
    r->state = BL_JOB_S_FINISHED;
    r->resume_link = 1; /* the job's last restart brings the link back */
    seal(r);
}

bl_job_boot_t bl_job_on_boot(bl_job_record_t* r) {
    if(r == NULL) return BL_JOB_BOOT_NONE;
    if(!bl_job_record_valid(r)) {
        bl_job_record_clear(r);
        return BL_JOB_BOOT_NONE;
    }
    switch(r->state) {
    case BL_JOB_S_PENDING:
        /* Sealed as RUNNING before the tool starts: if this boot dies,
         * the next one sees RUNNING and never runs it again. */
        r->state = BL_JOB_S_RUNNING;
        seal(r);
        return BL_JOB_BOOT_RUN;
    case BL_JOB_S_RUNNING:
        bl_job_record_finish(r, BL_JOB_INTERRUPTED, kInterrupted, sizeof(kInterrupted) - 1);
        break;
    default: break;
    }
    if(r->resume_link) {
        /* Once: cleared before BLE starts, so a crash while starting it,
         * or any later reboot, is a normal boot. */
        r->resume_link = 0;
        seal(r);
        return BL_JOB_BOOT_LINK;
    }
    return BL_JOB_BOOT_NONE;
}

/* ---- payloads ------------------------------------------------------- */

int bl_job_encode_start(
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len,
    uint8_t* out,
    size_t out_cap) {
    if(out == NULL || args_len > BL_JOB_ARGS_MAX || (args_len > 0 && args == NULL)) return BL_E_ARG;
    if(out_cap < 4 + args_len) return BL_E_OVERFLOW;
    put_le16(out, cap_id);
    put_le16(out + 2, max_s);
    if(args_len > 0) memcpy(out + 4, args, args_len);
    return (int)(4 + args_len);
}

int bl_job_parse_start(
    const uint8_t* p,
    size_t n,
    uint16_t* cap_id,
    uint16_t* max_s,
    const uint8_t** args,
    size_t* args_len) {
    if(p == NULL || cap_id == NULL || max_s == NULL || args == NULL || args_len == NULL) return BL_E_ARG;
    if(n < 4) return BL_E_SHORT;
    if(n - 4 > BL_JOB_ARGS_MAX) return BL_E_TOO_LARGE;
    *cap_id = get_le16(p);
    *max_s = get_le16(p + 2);
    *args = p + 4;
    *args_len = n - 4;
    return BL_OK;
}

int bl_job_encode_accepted(uint32_t job_id, uint16_t max_s, uint8_t* out, size_t out_cap) {
    if(out == NULL) return BL_E_ARG;
    if(out_cap < 6) return BL_E_OVERFLOW;
    put_le32(out, job_id);
    put_le16(out + 4, max_s);
    return 6;
}

int bl_job_parse_accepted(const uint8_t* p, size_t n, uint32_t* job_id, uint16_t* max_s) {
    if(p == NULL || job_id == NULL || max_s == NULL) return BL_E_ARG;
    if(n != 6) return n < 6 ? BL_E_SHORT : BL_E_FORMAT;
    *job_id = get_le32(p);
    *max_s = get_le16(p + 4);
    if(*job_id == 0 || *max_s == 0 || *max_s > BL_JOB_MAX_S) return BL_E_FORMAT;
    return BL_OK;
}

int bl_job_encode_result_req(uint32_t job_id, uint8_t* out, size_t out_cap) {
    if(out == NULL) return BL_E_ARG;
    if(out_cap < 4) return BL_E_OVERFLOW;
    put_le32(out, job_id);
    return 4;
}

int bl_job_parse_result_req(const uint8_t* p, size_t n, uint32_t* job_id) {
    if(p == NULL || job_id == NULL) return BL_E_ARG;
    if(n != 4) return n < 4 ? BL_E_SHORT : BL_E_FORMAT;
    *job_id = get_le32(p);
    return BL_OK;
}

int bl_job_encode_result(
    uint32_t job_id,
    uint8_t outcome,
    const char* text,
    size_t text_len,
    uint8_t* out,
    size_t out_cap) {
    if(out == NULL || (text_len > 0 && text == NULL)) return BL_E_ARG;
    if(text_len > BL_JOB_RESULT_MAX) return BL_E_TOO_LARGE;
    if(out_cap < 5 + text_len) return BL_E_OVERFLOW;
    put_le32(out, job_id);
    out[4] = outcome;
    if(text_len > 0) memcpy(out + 5, text, text_len);
    return (int)(5 + text_len);
}

int bl_job_parse_result(
    const uint8_t* p,
    size_t n,
    uint32_t* job_id,
    uint8_t* outcome,
    const char** text,
    size_t* text_len) {
    if(p == NULL || job_id == NULL || outcome == NULL || text == NULL || text_len == NULL) return BL_E_ARG;
    if(n < 5) return BL_E_SHORT;
    if(n - 5 > BL_JOB_RESULT_MAX) return BL_E_TOO_LARGE;
    *job_id = get_le32(p);
    *outcome = p[4];
    if(*outcome < BL_JOB_DONE || *outcome > BL_JOB_INTERRUPTED) return BL_E_FORMAT;
    *text = (const char*)(p + 5);
    *text_len = n - 5;
    return BL_OK;
}

const char* bl_job_outcome_str(uint8_t outcome) {
    switch(outcome) {
    case BL_JOB_DONE: return "done";
    case BL_JOB_FAILED: return "failed";
    case BL_JOB_STOPPED: return "stopped on the StickS3";
    case BL_JOB_TIME_LIMIT: return "time limit reached";
    case BL_JOB_INTERRUPTED: return "interrupted";
    default: return "unknown";
    }
}

/* ---- Flipper-side tracker ------------------------------------------- */

void bl_jobc_init(bl_jobc_t* j) {
    if(j == NULL) return;
    memset(j, 0, sizeof(*j));
}

static void jobc_fail(bl_jobc_t* j, bl_jobc_fail_t why, uint16_t err) {
    j->state = BL_JOBC_FAILED;
    j->fail = why;
    j->err = err;
}

int bl_jobc_start(
    bl_jobc_t* j,
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len,
    uint32_t now_ms,
    uint8_t* out,
    size_t out_cap) {
    if(j == NULL || max_s > BL_JOB_MAX_S) return BL_E_ARG;
    const int n = bl_job_encode_start(cap_id, max_s, args, args_len, out, out_cap);
    if(n < 0) return n;
    bl_jobc_init(j);
    j->state = BL_JOBC_STARTING;
    j->cap_id = cap_id;
    j->max_s = max_s;
    j->started_ms = now_ms;
    return n;
}

static uint16_t wire_err(const uint8_t* payload, size_t len) {
    return len >= 2 ? get_le16(payload) : (uint16_t)BL_ERR_INTERNAL;
}

bool bl_jobc_on_response(
    bl_jobc_t* j,
    uint8_t type,
    const uint8_t* payload,
    size_t len,
    uint32_t now_ms,
    const char** text,
    size_t* text_len) {
    if(j == NULL) return false;
    if(text) *text = NULL;
    if(text_len) *text_len = 0;
    if(j->state == BL_JOBC_STARTING) {
        if(type == BL_MSG_ERROR) {
            jobc_fail(j, BL_JOBC_F_REFUSED, wire_err(payload, len));
            return true;
        }
        if(type != BL_MSG_JOB_ACCEPTED) return false;
        uint32_t id = 0;
        uint16_t max_s = 0;
        if(bl_job_parse_accepted(payload, len, &id, &max_s) != BL_OK) {
            jobc_fail(j, BL_JOBC_F_PROTOCOL, 0);
            return true;
        }
        j->job_id = id;
        j->max_s = max_s;
        j->deadline_ms = now_ms + (uint32_t)max_s * 1000u + BL_JOB_RESTART_ALLOWANCE_MS;
        j->link_dropped = false;
        j->state = BL_JOBC_RUNNING;
        return true;
    }
    if(j->state == BL_JOBC_FETCHING) {
        if(type == BL_MSG_ERROR) {
            const uint16_t e = wire_err(payload, len);
            if(e == BL_ERR_BUSY) {
                j->state = BL_JOBC_RUNNING; /* not finished yet: ask again later */
                j->link_dropped = false; /* only after the next restart */
            } else {
                jobc_fail(j, BL_JOBC_F_LOST, e);
            }
            return true;
        }
        if(type != BL_MSG_JOB_RESULT) return false;
        uint32_t id = 0;
        uint8_t outcome = 0;
        const char* t = NULL;
        size_t tl = 0;
        if(bl_job_parse_result(payload, len, &id, &outcome, &t, &tl) != BL_OK || id != j->job_id) {
            jobc_fail(j, BL_JOBC_F_PROTOCOL, 0);
            return true;
        }
        j->outcome = outcome;
        j->state = BL_JOBC_DONE;
        if(text) *text = t;
        if(text_len) *text_len = tl;
        return true;
    }
    return false;
}

void bl_jobc_link_down(bl_jobc_t* j) {
    if(j == NULL) return;
    if(j->state == BL_JOBC_RUNNING) j->link_dropped = true;
    if(j->state == BL_JOBC_FETCHING) {
        j->state = BL_JOBC_RUNNING; /* the request went with the link */
        j->link_dropped = true;
    }
    if(j->state == BL_JOBC_STARTING) {
        /* The link went before the answer. The StickS3 may have accepted
         * and restarted, but without the job id the result cannot be
         * fetched: report it rather than wait. */
        jobc_fail(j, BL_JOBC_F_NO_ANSWER, 0);
    }
}

bool bl_jobc_want_fetch(const bl_jobc_t* j, bool session_ready) {
    return j != NULL && j->state == BL_JOBC_RUNNING && j->link_dropped && session_ready;
}

int bl_jobc_fetch(bl_jobc_t* j, uint8_t* out, size_t out_cap) {
    /* Never on the session that started the job (review 2026-11-23: this
     * relied on callers checking bl_jobc_want_fetch first). */
    if(j == NULL || j->state != BL_JOBC_RUNNING || !j->link_dropped) return BL_E_STATE;
    const int n = bl_job_encode_result_req(j->job_id, out, out_cap);
    if(n > 0) j->state = BL_JOBC_FETCHING;
    return n;
}

void bl_jobc_request_timed_out(bl_jobc_t* j) {
    if(j == NULL) return;
    if(j->state == BL_JOBC_STARTING) jobc_fail(j, BL_JOBC_F_NO_ANSWER, 0);
    if(j->state == BL_JOBC_FETCHING) j->state = BL_JOBC_RUNNING; /* ask again */
}

bool bl_jobc_tick(bl_jobc_t* j, uint32_t now_ms) {
    if(j == NULL) return false;
    if(j->state != BL_JOBC_RUNNING && j->state != BL_JOBC_FETCHING) return false;
    if((int32_t)(now_ms - j->deadline_ms) < 0) return false;
    jobc_fail(j, BL_JOBC_F_TIMEOUT, 0);
    return true;
}
