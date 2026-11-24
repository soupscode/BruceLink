/* Host tests for jobs (bl_job, ADR 0006). SPDX-License-Identifier: MIT */
#include "bl_capability.h"
#include "bl_job.h"
#include "bl_pcap.h"
#include "bl_proto.h"
#include "bl_test.h"

#include <stddef.h>

static const uint8_t kArgs[3] = {1, 2, 3};

static void t_record_create_and_bounds(void) {
    bl_job_record_t r;
    bl_job_record_clear(&r);
    CHECK(!bl_job_record_valid(&r)); /* zeroed = no job */
    CHECK(!bl_job_record_valid(NULL));
    CHECK_EQ_INT(bl_job_record_create(&r, 0, 0x10, 20, NULL, 0), BL_E_ARG); /* id 0 */
    CHECK_EQ_INT(bl_job_record_create(&r, 7, 0x10, 0, NULL, 0), BL_E_ARG); /* no time */
    CHECK_EQ_INT(bl_job_record_create(&r, 7, 0x10, BL_JOB_MAX_S + 1, NULL, 0), BL_E_ARG);
    uint8_t big[BL_JOB_ARGS_MAX + 1] = {0};
    CHECK_EQ_INT(bl_job_record_create(&r, 7, 0x10, 20, big, sizeof(big)), BL_E_ARG);
    CHECK_EQ_INT(bl_job_record_create(&r, 7, 0x10, 20, NULL, 2), BL_E_ARG);
    CHECK_EQ_INT(bl_job_record_create(NULL, 7, 0x10, 20, NULL, 0), BL_E_ARG);

    CHECK_EQ_INT(bl_job_record_create(&r, 7, 0x10, BL_JOB_MAX_S, big, BL_JOB_ARGS_MAX), BL_OK);
    CHECK(bl_job_record_valid(&r));
    CHECK_EQ_INT(bl_job_record_create(&r, 0xDEADBEEFu, 0x10, 20, kArgs, sizeof(kArgs)), BL_OK);
    CHECK(bl_job_record_valid(&r));
    CHECK_EQ_INT(r.state, BL_JOB_S_PENDING);
    CHECK_EQ_INT(r.args_len, 3);
    CHECK(memcmp(r.args, kArgs, 3) == 0);
    /* the layout has no padding: the CRC covers every byte before it */
    CHECK_EQ_INT(offsetof(bl_job_record_t, crc), sizeof(bl_job_record_t) - 4);
}

static void t_record_detects_any_corruption(void) {
    bl_job_record_t r;
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, kArgs, sizeof(kArgs)), BL_OK);
    bl_job_record_finish(&r, BL_JOB_DONE, "3 networks", 10);
    uint8_t* b = (uint8_t*)&r;
    int caught = 0;
    const size_t n = sizeof(r);
    for(size_t i = 0; i < n; i++) {
        b[i] ^= 0x01;
        if(!bl_job_record_valid(&r)) caught++;
        b[i] ^= 0x01;
    }
    CHECK_EQ_INT(caught, (long)n); /* every single-bit flip, CRC field included */
    CHECK(bl_job_record_valid(&r));

    /* what RTC memory holds after a power-on: anything */
    bl_job_record_t g;
    uint32_t s = 0x1234567u;
    for(int trial = 0; trial < 200; trial++) {
        uint8_t* gb = (uint8_t*)&g;
        for(size_t i = 0; i < sizeof(g); i++) {
            s = s * 1103515245u + 12345u;
            gb[i] = (uint8_t)(s >> 16);
        }
        if(trial % 2) g.magic = BL_JOB_MAGIC; /* even with the right magic */
        CHECK(!bl_job_record_valid(&g));
        CHECK_EQ_INT(bl_job_on_boot(&g), BL_JOB_BOOT_NONE);
        CHECK(!bl_job_record_valid(&g));
        CHECK_EQ_INT(g.magic, 0); /* cleared */
    }

    /* a correctly sealed record with an out-of-range field is still refused */
    const size_t body = offsetof(bl_job_record_t, crc);
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    r.state = 9; /* unknown state: must never run */
    r.crc = bl_crc32(&r, body);
    CHECK(!bl_job_record_valid(&r));
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_NONE);
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    r.args_len = BL_JOB_ARGS_MAX + 1;
    r.crc = bl_crc32(&r, body);
    CHECK(!bl_job_record_valid(&r));
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    bl_job_record_finish(&r, BL_JOB_DONE, "x", 1);
    r.outcome = 0;
    r.crc = bl_crc32(&r, body);
    CHECK(!bl_job_record_valid(&r));
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    r.max_s = BL_JOB_MAX_S + 1;
    r.crc = bl_crc32(&r, body);
    CHECK(!bl_job_record_valid(&r));
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    r.resume_link = 2;
    r.crc = bl_crc32(&r, body);
    CHECK(!bl_job_record_valid(&r));
}

static void t_boot_normal_job(void) {
    bl_job_record_t r;
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    /* the restart after JOB_ACCEPTED */
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_RUN);
    CHECK_EQ_INT(r.state, BL_JOB_S_RUNNING);
    CHECK(bl_job_record_valid(&r));
    bl_job_record_finish(&r, BL_JOB_DONE, "3 networks", 10);
    CHECK(bl_job_record_valid(&r));
    /* the restart after the job: bring the link back, once */
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_LINK);
    CHECK(bl_job_record_valid(&r));
    CHECK_EQ_INT(r.resume_link, 0);
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_NONE); /* a later reboot */
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_NONE);
    /* the result is still there to fetch */
    CHECK(bl_job_record_valid(&r));
    CHECK_EQ_INT(r.state, BL_JOB_S_FINISHED);
    CHECK_EQ_INT(r.outcome, BL_JOB_DONE);
    CHECK_EQ_INT(r.result_len, 10);
    CHECK(memcmp(r.result, "3 networks", 10) == 0);
}

static void t_boot_crash_mid_job_never_reruns(void) {
    bl_job_record_t r;
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_RUN);
    /* the tool crashes the StickS3: the next boot finds RUNNING */
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_LINK);
    CHECK_EQ_INT(r.state, BL_JOB_S_FINISHED);
    CHECK_EQ_INT(r.outcome, BL_JOB_INTERRUPTED);
    CHECK(r.result_len > 0);
    CHECK(bl_job_record_valid(&r));
    /* and the link start crashes too: still no re-run, no loop */
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_NONE);

    /* whatever happens, a record runs at most once */
    CHECK_EQ_INT(bl_job_record_create(&r, 43, 0x10, 20, NULL, 0), BL_OK);
    int runs = 0;
    for(int boot = 0; boot < 10; boot++)
        if(bl_job_on_boot(&r) == BL_JOB_BOOT_RUN) runs++;
    CHECK_EQ_INT(runs, 1);
}

static void t_record_survives_copy_and_truncates(void) {
    bl_job_record_t r, copy;
    CHECK_EQ_INT(bl_job_record_create(&r, 42, 0x10, 20, NULL, 0), BL_OK);
    CHECK_EQ_INT(bl_job_on_boot(&r), BL_JOB_BOOT_RUN);
    char text[BL_JOB_RESULT_MAX + 50];
    memset(text, 'x', sizeof(text));
    bl_job_record_finish(&r, BL_JOB_DONE, text, sizeof(text));
    CHECK_EQ_INT(r.result_len, BL_JOB_RESULT_MAX);
    memcpy(&copy, &r, sizeof(r)); /* what a restart does: the bytes stay */
    CHECK(bl_job_record_valid(&copy));
    bl_job_record_finish(&r, BL_JOB_FAILED, NULL, 5);
    CHECK_EQ_INT(r.result_len, 0);
    CHECK(bl_job_record_valid(&r));
}

static void t_payloads(void) {
    uint8_t b[BL_MAX_PAYLOAD];
    int n = bl_job_encode_start(0x0010, 20, kArgs, 3, b, sizeof(b));
    CHECK_EQ_INT(n, 7);
    CHECK(b[0] == 0x10 && b[1] == 0x00 && b[2] == 20 && b[3] == 0 && b[4] == 1);
    uint16_t cap = 0, ms = 0;
    const uint8_t* a = NULL;
    size_t al = 0;
    CHECK_EQ_INT(bl_job_parse_start(b, (size_t)n, &cap, &ms, &a, &al), BL_OK);
    CHECK(cap == 0x10 && ms == 20 && al == 3 && a[2] == 3);
    CHECK_EQ_INT(bl_job_parse_start(b, 3, &cap, &ms, &a, &al), BL_E_SHORT);
    CHECK_EQ_INT(bl_job_parse_start(b, 4 + BL_JOB_ARGS_MAX + 1, &cap, &ms, &a, &al), BL_E_TOO_LARGE);
    CHECK_EQ_INT(bl_job_encode_start(1, 1, kArgs, 3, b, 6), BL_E_OVERFLOW);
    CHECK_EQ_INT(bl_job_encode_start(1, 1, NULL, 3, b, sizeof(b)), BL_E_ARG);

    uint32_t id = 0;
    CHECK_EQ_INT(bl_job_encode_accepted(0xA1B2C3D4u, 30, b, sizeof(b)), 6);
    CHECK_EQ_INT(bl_job_parse_accepted(b, 6, &id, &ms), BL_OK);
    CHECK(id == 0xA1B2C3D4u && ms == 30);
    CHECK_EQ_INT(bl_job_parse_accepted(b, 5, &id, &ms), BL_E_SHORT);
    CHECK_EQ_INT(bl_job_parse_accepted(b, 7, &id, &ms), BL_E_FORMAT);
    bl_job_encode_accepted(0, 30, b, sizeof(b));
    CHECK_EQ_INT(bl_job_parse_accepted(b, 6, &id, &ms), BL_E_FORMAT); /* id 0 */
    bl_job_encode_accepted(5, BL_JOB_MAX_S + 1, b, sizeof(b));
    CHECK_EQ_INT(bl_job_parse_accepted(b, 6, &id, &ms), BL_E_FORMAT);
    bl_job_encode_accepted(5, 0, b, sizeof(b));
    CHECK_EQ_INT(bl_job_parse_accepted(b, 6, &id, &ms), BL_E_FORMAT);

    CHECK_EQ_INT(bl_job_encode_result_req(77, b, sizeof(b)), 4);
    CHECK_EQ_INT(bl_job_parse_result_req(b, 4, &id), BL_OK);
    CHECK_EQ_INT(id, 77);
    CHECK_EQ_INT(bl_job_parse_result_req(b, 3, &id), BL_E_SHORT);
    CHECK_EQ_INT(bl_job_parse_result_req(b, 5, &id), BL_E_FORMAT);

    char text[BL_JOB_RESULT_MAX + 1];
    memset(text, 'n', sizeof(text));
    uint8_t oc = 0;
    const char* t = NULL;
    size_t tl = 0;
    n = bl_job_encode_result(77, BL_JOB_DONE, text, BL_JOB_RESULT_MAX, b, sizeof(b));
    CHECK_EQ_INT(n, 5 + BL_JOB_RESULT_MAX); /* fits a frame: 485 <= 512 */
    CHECK(n <= (int)BL_MAX_PAYLOAD);
    CHECK_EQ_INT(bl_job_parse_result(b, (size_t)n, &id, &oc, &t, &tl), BL_OK);
    CHECK(id == 77 && oc == BL_JOB_DONE && tl == BL_JOB_RESULT_MAX && t[0] == 'n');
    CHECK_EQ_INT(bl_job_encode_result(77, 1, text, BL_JOB_RESULT_MAX + 1, b, sizeof(b)), BL_E_TOO_LARGE);
    CHECK_EQ_INT(bl_job_parse_result(b, 5 + BL_JOB_RESULT_MAX + 1, &id, &oc, &t, &tl), BL_E_TOO_LARGE);
    CHECK_EQ_INT(bl_job_parse_result(b, 4, &id, &oc, &t, &tl), BL_E_SHORT);
    b[4] = 0;
    CHECK_EQ_INT(bl_job_parse_result(b, 5, &id, &oc, &t, &tl), BL_E_FORMAT);
    b[4] = 6;
    CHECK_EQ_INT(bl_job_parse_result(b, 5, &id, &oc, &t, &tl), BL_E_FORMAT);
    CHECK(strcmp(bl_job_outcome_str(BL_JOB_INTERRUPTED), "interrupted") == 0);
    CHECK(strcmp(bl_job_outcome_str(0), "unknown") == 0);
    CHECK(strcmp(bl_msg_type_str(BL_MSG_JOB_RESULT), "JOB_RESULT") == 0);
    CHECK(strcmp(bl_wire_err_str(BL_ERR_USE_JOB), "runs as a job") == 0);
}

/* The StickS3's side of a reply, as a payload. */
static size_t accepted(uint8_t* b, uint32_t id, uint16_t max_s) {
    return (size_t)bl_job_encode_accepted(id, max_s, b, 16);
}
static size_t error_pl(uint8_t* b, uint16_t e) {
    b[0] = (uint8_t)(e & 0xFF);
    b[1] = (uint8_t)(e >> 8);
    return 2;
}

static void t_jobc_happy_path(void) {
    bl_jobc_t j;
    bl_jobc_init(&j);
    uint8_t out[64], in[BL_MAX_PAYLOAD];
    const uint32_t t0 = 1000;
    CHECK_EQ_INT(bl_jobc_start(&j, BL_CAP_WIFI_SCAN, 20, NULL, 0, t0, out, sizeof(out)), 4);
    CHECK_EQ_INT(j.state, BL_JOBC_STARTING);
    CHECK(!bl_jobc_want_fetch(&j, true));
    /* unrelated responses are not the job's */
    CHECK(!bl_jobc_on_response(&j, BL_MSG_DEVICE_INFO, in, 0, t0, NULL, NULL));

    size_t n = accepted(in, 99, 20);
    CHECK(bl_jobc_on_response(&j, BL_MSG_JOB_ACCEPTED, in, n, t0 + 100, NULL, NULL));
    CHECK_EQ_INT(j.state, BL_JOBC_RUNNING);
    CHECK_EQ_INT(j.job_id, 99);
    /* never fetch on the session that started it: the StickS3 has not
     * restarted yet */
    CHECK(!bl_jobc_want_fetch(&j, true));
    bl_jobc_link_down(&j);
    CHECK(!bl_jobc_want_fetch(&j, false)); /* no session yet */
    CHECK(bl_jobc_want_fetch(&j, true));
    CHECK_EQ_INT(bl_jobc_fetch(&j, out, sizeof(out)), 4);
    CHECK_EQ_INT(j.state, BL_JOBC_FETCHING);
    CHECK(!bl_jobc_want_fetch(&j, true));

    n = (size_t)bl_job_encode_result(99, BL_JOB_DONE, "2 networks", 10, in, sizeof(in));
    const char* t = NULL;
    size_t tl = 0;
    CHECK(bl_jobc_on_response(&j, BL_MSG_JOB_RESULT, in, n, t0 + 30000, &t, &tl));
    CHECK_EQ_INT(j.state, BL_JOBC_DONE);
    CHECK_EQ_INT(j.outcome, BL_JOB_DONE);
    CHECK(tl == 10 && memcmp(t, "2 networks", 10) == 0);
    CHECK(!bl_jobc_tick(&j, t0 + 10000000u)); /* done: no timeout */
}

static void t_jobc_refusals_and_losses(void) {
    bl_jobc_t j;
    uint8_t out[64], in[BL_MAX_PAYLOAD];
    /* refused */
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    size_t n = error_pl(in, BL_ERR_NOT_AUTHORIZED);
    CHECK(bl_jobc_on_response(&j, BL_MSG_ERROR, in, n, 1, NULL, NULL));
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_REFUSED && j.err == BL_ERR_NOT_AUTHORIZED);
    /* no answer: request timeout, or the link went first */
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    bl_jobc_request_timed_out(&j);
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_NO_ANSWER);
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    bl_jobc_link_down(&j);
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_NO_ANSWER);
    /* malformed acceptance */
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    CHECK(bl_jobc_on_response(&j, BL_MSG_JOB_ACCEPTED, in, 3, 1, NULL, NULL));
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_PROTOCOL);

    /* the StickS3 lost the job (power-off): NOT_FOUND */
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    n = accepted(in, 5, 20);
    bl_jobc_on_response(&j, BL_MSG_JOB_ACCEPTED, in, n, 1, NULL, NULL);
    bl_jobc_link_down(&j);
    bl_jobc_fetch(&j, out, sizeof(out));
    n = error_pl(in, BL_ERR_NOT_FOUND);
    CHECK(bl_jobc_on_response(&j, BL_MSG_ERROR, in, n, 2, NULL, NULL));
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_LOST && j.err == BL_ERR_NOT_FOUND);

    /* a result for another job is not accepted as this one's */
    bl_jobc_start(&j, 0x10, 0, NULL, 0, 0, out, sizeof(out));
    n = accepted(in, 5, 20);
    bl_jobc_on_response(&j, BL_MSG_JOB_ACCEPTED, in, n, 1, NULL, NULL);
    bl_jobc_link_down(&j);
    bl_jobc_fetch(&j, out, sizeof(out));
    n = (size_t)bl_job_encode_result(6, BL_JOB_DONE, "x", 1, in, sizeof(in));
    CHECK(bl_jobc_on_response(&j, BL_MSG_JOB_RESULT, in, n, 2, NULL, NULL));
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_PROTOCOL);
}

static void t_jobc_retries_and_timeout(void) {
    bl_jobc_t j;
    uint8_t out[64], in[64];
    const uint32_t t0 = 0xFFFF0000u; /* the deadline wraps past 2^32 */
    bl_jobc_start(&j, 0x10, 20, NULL, 0, t0, out, sizeof(out));
    size_t n = accepted(in, 5, 20);
    bl_jobc_on_response(&j, BL_MSG_JOB_ACCEPTED, in, n, t0, NULL, NULL);
    const uint32_t deadline = t0 + 20000u + BL_JOB_RESTART_ALLOWANCE_MS;
    CHECK_EQ_INT(j.deadline_ms, deadline);
    bl_jobc_link_down(&j);
    /* a fetch lost with the link is asked again on the next session */
    bl_jobc_fetch(&j, out, sizeof(out));
    bl_jobc_link_down(&j);
    CHECK_EQ_INT(j.state, BL_JOBC_RUNNING);
    CHECK(bl_jobc_want_fetch(&j, true));
    /* a fetch that timed out is asked again too */
    bl_jobc_fetch(&j, out, sizeof(out));
    bl_jobc_request_timed_out(&j);
    CHECK(bl_jobc_want_fetch(&j, true));
    /* BUSY (not finished): wait for the next restart */
    bl_jobc_fetch(&j, out, sizeof(out));
    n = error_pl(in, BL_ERR_BUSY);
    CHECK(bl_jobc_on_response(&j, BL_MSG_ERROR, in, n, t0 + 5, NULL, NULL));
    CHECK_EQ_INT(j.state, BL_JOBC_RUNNING);
    CHECK(!bl_jobc_want_fetch(&j, true));
    /* gives up exactly at the deadline, once */
    CHECK(!bl_jobc_tick(&j, deadline - 1));
    CHECK(bl_jobc_tick(&j, deadline));
    CHECK(j.state == BL_JOBC_FAILED && j.fail == BL_JOBC_F_TIMEOUT);
    CHECK(!bl_jobc_tick(&j, deadline + 1));
    CHECK_EQ_INT(bl_jobc_elapsed_ms(&j, t0 + 1500u), 1500);
    CHECK_EQ_INT(bl_jobc_start(&j, 0x10, BL_JOB_MAX_S + 1, NULL, 0, 0, out, sizeof(out)), BL_E_ARG);
}

void run_job_tests(void) {
    t_record_create_and_bounds();
    t_record_detects_any_corruption();
    t_boot_normal_job();
    t_boot_crash_mid_job_never_reruns();
    t_record_survives_copy_and_truncates();
    t_payloads();
    t_jobc_happy_path();
    t_jobc_refusals_and_losses();
    t_jobc_retries_and_timeout();
    printf("  9 tests ok\n");
}
