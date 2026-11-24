/* BruceLink host test runner. SPDX-License-Identifier: MIT */
#include "bl_test.h"

int bl_tests_run = 0;
int bl_tests_failed = 0;

void run_crypto_tests(void);
void run_protocol_tests(void);
void run_capability_tests(void);
void run_integration_tests(void);
void run_textbuf_tests(void);
void run_client_tests(void);
void run_gatt_evt_tests(void);
void run_pair_tests(void);
void run_job_tests(void);
void run_files_tests(void);

int main(void) {
    printf("BruceLink core tests\n");
    printf("[crypto]\n");
    run_crypto_tests();
    printf("[protocol]\n");
    run_protocol_tests();
    printf("[capability+capture]\n");
    run_capability_tests();
    printf("[integration]\n");
    run_integration_tests();
    printf("[textbuf]\n");
    run_textbuf_tests();
    printf("[client]\n");
    run_client_tests();
    printf("[gatt-evt]\n");
    run_gatt_evt_tests();
    printf("[pair]\n");
    run_pair_tests();
    printf("[job]\n");
    run_job_tests();
    printf("[files]\n");
    run_files_tests();
    printf("\n%d checks, %d failed\n", bl_tests_run, bl_tests_failed);
    return bl_tests_failed == 0 ? 0 : 1;
}
