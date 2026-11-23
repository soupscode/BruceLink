/*
 * BruceLink shared core — common types and limits.
 * Portable C99, no dynamic allocation. Used by the Flipper app, the
 * ESP32-S3 bridge and the host simulators/tests.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wire format version carried in every message header. */
#define BL_WIRE_VERSION 1u
/* Application protocol revisions this implementation supports. */
#define BL_PROTO_MIN 1u
#define BL_PROTO_MAX 1u

#define BL_HDR_LEN 12u
#define BL_MAC_LEN 16u
#define BL_MAX_PAYLOAD 512u
#define BL_MAX_MSG (BL_HDR_LEN + BL_MAX_PAYLOAD + BL_MAC_LEN)

#define BL_KEY_LEN 32u
#define BL_ID_LEN 8u
#define BL_NONCE_LEN 16u

/* Internal result codes (never sent on the wire). */
typedef enum {
    BL_OK = 0,
    BL_E_ARG = -1, /* bad argument */
    BL_E_SHORT = -2, /* input truncated */
    BL_E_OVERFLOW = -3, /* output buffer too small */
    BL_E_FORMAT = -4, /* malformed input */
    BL_E_VERSION = -5, /* unsupported wire version */
    BL_E_MAC = -6, /* authentication tag mismatch */
    BL_E_REPLAY = -7, /* counter not strictly increasing */
    BL_E_STATE = -8, /* message not valid in current state */
    BL_E_CRC = -9, /* stream CRC mismatch */
    BL_E_TOO_LARGE = -10, /* exceeds protocol limits */
    BL_E_AUTH = -11, /* handshake proof failed */
    BL_E_CRYPTO = -12, /* crypto failure (e.g. low-order point) */
    BL_E_SEQ = -13, /* fragment sequence error */
    BL_E_LOCKED = -14, /* temporarily locked out / rate limited */
    BL_E_DENIED = -15, /* operation not permitted (e.g. pairing closed) */
    BL_E_BUSY = -16, /* resource already in use (e.g. capture running) */
    BL_E_NO_RESOURCE = -17, /* out of a required resource (e.g. storage) */
} bl_result_t;

const char* bl_result_str(int r);

#ifdef __cplusplus
}
#endif
