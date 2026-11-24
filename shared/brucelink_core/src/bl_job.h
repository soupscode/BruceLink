/*
 * BruceLink shared core — jobs: tools that run between two restarts of the
 * StickS3, with the BLE link down (ADR 0006).
 *
 * Three parts, all pure logic (host-tested):
 *   - the job record the StickS3 keeps in RTC memory across restarts, with
 *     the boot-time state machine that guarantees a job is never re-run;
 *   - the JOB_* payload layouts (shared/protocol.md);
 *   - the Flipper-side tracker that follows one job through the link drop
 *     and fetches its result on the next session.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BL_JOB_ARGS_MAX 32u
#define BL_JOB_RESULT_MAX 480u
/* Upper bound for any job's time limit, in seconds. */
#define BL_JOB_MAX_S 300u
/* Flipper side: time allowed on top of the job's limit for two restarts of
 * the StickS3, the BLE reconnect and the handshake. An estimate until
 * measured on hardware. */
#define BL_JOB_RESTART_ALLOWANCE_MS 60000u

#define BL_JOB_MAGIC 0x424C4A42u /* "BLJB" */
#define BL_JOB_VERSION 1u

/* Outcome of a finished job (JOB_RESULT). */
typedef enum {
    BL_JOB_DONE = 1, /* the tool ran; text is its result */
    BL_JOB_FAILED = 2, /* the tool reported an error; text says what */
    BL_JOB_STOPPED = 3, /* stopped with a button on the StickS3 */
    BL_JOB_TIME_LIMIT = 4, /* ended by its time limit */
    BL_JOB_INTERRUPTED = 5, /* the StickS3 reset during the job; not re-run */
} bl_job_outcome_t;

/* Record state. */
typedef enum {
    BL_JOB_S_NONE = 0,
    BL_JOB_S_PENDING = 1, /* accepted, the StickS3 is about to restart */
    BL_JOB_S_RUNNING = 2, /* sealed before the tool starts */
    BL_JOB_S_FINISHED = 3, /* result stored */
} bl_job_state_t;

/* The record kept in memory that survives a restart (RTC_NOINIT on the
 * StickS3). Fixed layout, no padding; the CRC covers everything before it.
 * Only valid if magic, version, CRC and bounds all check out. */
typedef struct {
    uint32_t magic;
    uint32_t job_id;
    uint16_t version;
    uint16_t cap_id;
    uint16_t max_s;
    uint16_t result_len;
    uint8_t state; /* bl_job_state_t */
    uint8_t outcome; /* bl_job_outcome_t, when FINISHED */
    uint8_t resume_link; /* start BruceLink (BLE) at the next boot */
    uint8_t args_len;
    uint8_t args[BL_JOB_ARGS_MAX];
    char result[BL_JOB_RESULT_MAX]; /* text, not NUL-terminated */
    uint32_t crc;
} bl_job_record_t;

/* Make the record invalid (no job). */
void bl_job_record_clear(bl_job_record_t* r);
/* A new job, PENDING, to run at the next boot. job_id must not be 0;
 * 1 <= max_s <= BL_JOB_MAX_S; args_len <= BL_JOB_ARGS_MAX. Sealed. */
int bl_job_record_create(
    bl_job_record_t* r,
    uint32_t job_id,
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len);
/* Magic, version, CRC and every bound. */
bool bl_job_record_valid(const bl_job_record_t* r);
/* Store the result (text cut at BL_JOB_RESULT_MAX), FINISHED, and ask for
 * BruceLink at the next boot. Sealed. */
void bl_job_record_finish(bl_job_record_t* r, bl_job_outcome_t outcome, const char* text, size_t len);

typedef enum {
    BL_JOB_BOOT_NONE = 0, /* nothing to do: a normal boot */
    BL_JOB_BOOT_RUN, /* run the job now (state is RUNNING), then finish + restart */
    BL_JOB_BOOT_LINK, /* start BruceLink: a job's result is waiting */
} bl_job_boot_t;

/*
 * Call once per boot, before anything else touches the record. Decides
 * and records the transition, so a reset at any point never runs a job
 * twice:
 *   invalid                     -> cleared, NONE
 *   PENDING                     -> RUNNING, RUN
 *   RUNNING (reset mid-job)     -> FINISHED/INTERRUPTED, then as FINISHED
 *   FINISHED with resume_link   -> resume_link cleared, LINK
 *   FINISHED without            -> NONE (result kept for fetching)
 */
bl_job_boot_t bl_job_on_boot(bl_job_record_t* r);

/* ---- payloads (shared/protocol.md) --------------------------------- */

/* JOB_START: cap_id u16 | max_s u16 (0 = the capability's default) | args */
int bl_job_encode_start(
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len,
    uint8_t* out,
    size_t out_cap);
int bl_job_parse_start(
    const uint8_t* p,
    size_t n,
    uint16_t* cap_id,
    uint16_t* max_s,
    const uint8_t** args,
    size_t* args_len);
/* JOB_ACCEPTED: job_id u32 | max_s u16 */
int bl_job_encode_accepted(uint32_t job_id, uint16_t max_s, uint8_t* out, size_t out_cap);
int bl_job_parse_accepted(const uint8_t* p, size_t n, uint32_t* job_id, uint16_t* max_s);
/* JOB_RESULT_REQ: job_id u32 */
int bl_job_encode_result_req(uint32_t job_id, uint8_t* out, size_t out_cap);
int bl_job_parse_result_req(const uint8_t* p, size_t n, uint32_t* job_id);
/* JOB_RESULT: job_id u32 | outcome u8 | text */
int bl_job_encode_result(
    uint32_t job_id,
    uint8_t outcome,
    const char* text,
    size_t text_len,
    uint8_t* out,
    size_t out_cap);
int bl_job_parse_result(
    const uint8_t* p,
    size_t n,
    uint32_t* job_id,
    uint8_t* outcome,
    const char** text,
    size_t* text_len);

const char* bl_job_outcome_str(uint8_t outcome);

/* ---- Flipper side: follow one job ----------------------------------- */

typedef enum {
    BL_JOBC_IDLE = 0,
    BL_JOBC_STARTING, /* JOB_START sent, waiting for JOB_ACCEPTED */
    BL_JOBC_RUNNING, /* accepted; waiting for the link to drop and return */
    BL_JOBC_FETCHING, /* JOB_RESULT_REQ sent on a new session */
    BL_JOBC_DONE, /* result received (outcome set) */
    BL_JOBC_FAILED, /* see fail / err */
} bl_jobc_state_t;

typedef enum {
    BL_JOBC_F_NONE = 0,
    BL_JOBC_F_REFUSED, /* the StickS3 refused the job (err) */
    BL_JOBC_F_NO_ANSWER, /* no reply to JOB_START */
    BL_JOBC_F_TIMEOUT, /* the StickS3 did not come back in time */
    BL_JOBC_F_LOST, /* the StickS3 no longer has this job (err NOT_FOUND) */
    BL_JOBC_F_PROTOCOL, /* malformed reply */
} bl_jobc_fail_t;

typedef struct {
    bl_jobc_state_t state;
    bl_jobc_fail_t fail;
    uint16_t err; /* wire error with F_REFUSED / F_LOST */
    uint16_t cap_id;
    uint16_t max_s;
    uint32_t job_id;
    uint32_t started_ms; /* JOB_START sent */
    uint32_t deadline_ms; /* while RUNNING / FETCHING */
    bool link_dropped; /* the starting session has ended */
    uint8_t outcome; /* when DONE */
} bl_jobc_t;

void bl_jobc_init(bl_jobc_t* j);
/* Begin a job: writes the JOB_START payload (send it as a request) and
 * returns its length, or a negative bl_result_t. */
int bl_jobc_start(
    bl_jobc_t* j,
    uint16_t cap_id,
    uint16_t max_s,
    const uint8_t* args,
    size_t args_len,
    uint32_t now_ms,
    uint8_t* out,
    size_t out_cap);
/* A response arrived (type, payload). Returns true if it was the job's.
 * With JOB_RESULT, *text / *text_len point into payload. */
bool bl_jobc_on_response(
    bl_jobc_t* j,
    uint8_t type,
    const uint8_t* payload,
    size_t len,
    uint32_t now_ms,
    const char** text,
    size_t* text_len);
/* The link dropped (the StickS3 is restarting). */
void bl_jobc_link_down(bl_jobc_t* j);
/* True when a result request should be sent now: accepted, the starting
 * session has ended, and a new one is ready. */
bool bl_jobc_want_fetch(const bl_jobc_t* j, bool session_ready);
/* Writes the JOB_RESULT_REQ payload and moves to FETCHING. */
int bl_jobc_fetch(bl_jobc_t* j, uint8_t* out, size_t out_cap);
/* The client reported that the outstanding request timed out. */
void bl_jobc_request_timed_out(bl_jobc_t* j);
/* Advance time. Returns true once, when the job gives up (F_TIMEOUT). */
bool bl_jobc_tick(bl_jobc_t* j, uint32_t now_ms);
/* Milliseconds since JOB_START was sent. */
static inline uint32_t bl_jobc_elapsed_ms(const bl_jobc_t* j, uint32_t now_ms) {
    return now_ms - j->started_ms;
}

#ifdef __cplusplus
}
#endif
