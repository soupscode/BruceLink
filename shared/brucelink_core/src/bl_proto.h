/*
 * BruceLink shared core — protocol v1 message definitions.
 *
 * A logical message is: header(12) || payload(0..512) || mac(16).
 * The MAC is HMAC-SHA256(session_rx/tx_key, header||payload) truncated
 * to 16 bytes. Handshake messages carry a zero MAC field (their
 * integrity comes from the embedded confirm tags); all post-handshake
 * messages are authenticated.
 *
 * Multi-byte integers are little-endian.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Header field offsets. */
#define BL_OFF_MAGIC0 0
#define BL_OFF_MAGIC1 1
#define BL_OFF_VERSION 2
#define BL_OFF_TYPE 3
#define BL_OFF_FLAGS 4
#define BL_OFF_RSVD 5
#define BL_OFF_SEQ 6 /* u32 LE: per-session monotonic counter */
#define BL_OFF_REQID 10 /* u16 LE: request/response correlation (part of seq? no) */
/* NOTE: header is 12 bytes: magic(2) ver(1) type(1) flags(1) rsvd(1) seq(4) reqid(2) lenHi?
 * Layout (12 bytes):
 *   [0]=B [1]=L [2]=ver [3]=type [4]=flags [5]=reserved
 *   [6..9]=seq (u32 LE) [10..11]=payload_len (u16 LE)
 * request_id is carried in the first 2 payload bytes for request/response
 * message types (see bl_msg_* helpers), keeping the header fixed-size.
 */
#define BL_OFF_LEN 10

#define BL_MAGIC0 'B'
#define BL_MAGIC1 'L'

/* Header flags. */
#define BL_FLAG_NONE 0x00
#define BL_FLAG_RESPONSE 0x01 /* message is a response to a request */
#define BL_FLAG_ERROR 0x02 /* response carries an error payload */
#define BL_FLAG_EVENT 0x04 /* unsolicited event/notification */

/* Message types. 0x00-0x0F control; 0x10-0x3F info; 0x40-0x6F capture;
 * 0x70-0x7E reserved; 0x7F error. */
typedef enum {
    /* --- handshake / control (unauthenticated or self-authenticating) --- */
    BL_MSG_HELLO = 0x01, /* C->P: proto range + eph pub + nonce */
    BL_MSG_HELLO_ACK = 0x02, /* P->C: proto sel + eph pub + nonce + confirm_P */
    BL_MSG_AUTH = 0x03, /* C->P: confirm_C */
    BL_MSG_AUTH_OK = 0x04, /* P->C: session established */

    /* --- authenticated control --- */
    BL_MSG_PING = 0x05,
    BL_MSG_PONG = 0x06,
    BL_MSG_BYE = 0x07, /* graceful session teardown */

    /* --- pairing (bl_pair; zero MAC, no session) --- */
    BL_MSG_PAIR_COMMIT = 0x08, /* I->R: commitment to the initiator's key */
    BL_MSG_PAIR_KEY = 0x09, /* R->I: responder's key + nonce + name */
    BL_MSG_PAIR_REVEAL = 0x0A, /* I->R: initiator's key + nonce */
    BL_MSG_PAIR_CONFIRM = 0x0B, /* both: key confirmation after the user accepts */
    BL_MSG_PAIR_ABORT = 0x0C, /* both: rejected / cancelled / timed out */

    /* --- info --- */
    BL_MSG_DEVICE_INFO_REQ = 0x10,
    BL_MSG_DEVICE_INFO = 0x11,
    BL_MSG_STATUS_REQ = 0x12,
    BL_MSG_STATUS = 0x13,
    BL_MSG_CAPS_REQ = 0x14, /* capability registry query */
    BL_MSG_CAPS = 0x15,

    /* --- generalized command adapter --- */
    BL_MSG_CMD_REQ = 0x20, /* invoke a registered capability by id */
    BL_MSG_CMD_RESP = 0x21,

    /* --- capture subsystem (phase 2) --- */
    BL_MSG_CAP_START = 0x40,
    BL_MSG_CAP_STOP = 0x41,
    BL_MSG_CAP_STATUS = 0x42,
    BL_MSG_CAP_LIST = 0x43,
    BL_MSG_CAP_FILE_INFO = 0x44,
    BL_MSG_CAP_XFER_BEGIN = 0x45, /* begin resumable file transfer */
    BL_MSG_CAP_XFER_DATA = 0x46,
    BL_MSG_CAP_XFER_END = 0x47,
    BL_MSG_CAP_DELETE = 0x48,

    /* --- safety --- */
    BL_MSG_ESTOP = 0x7E, /* emergency stop: abort all active TX/capture */

    BL_MSG_ERROR = 0x7F,
} bl_msg_type_t;

/* Wire error codes carried in BL_MSG_ERROR / error responses (u16 LE). */
typedef enum {
    BL_ERR_NONE = 0,
    BL_ERR_BAD_FRAME = 1,
    BL_ERR_UNSUPPORTED_VERSION = 2,
    BL_ERR_NOT_AUTHENTICATED = 3,
    BL_ERR_UNKNOWN_TYPE = 4,
    BL_ERR_BAD_ARG = 5,
    BL_ERR_UNKNOWN_CAPABILITY = 6,
    BL_ERR_CAPABILITY_DISABLED = 7, /* e.g. active TX not authorized */
    BL_ERR_NOT_AUTHORIZED = 8, /* authorization token missing/invalid */
    BL_ERR_TARGET_NOT_ALLOWED = 9, /* target not on allowlist */
    BL_ERR_RATE_LIMITED = 10,
    BL_ERR_BUSY = 11,
    BL_ERR_NO_RESOURCE = 12, /* e.g. out of flash for capture */
    BL_ERR_NOT_FOUND = 13,
    BL_ERR_ESTOPPED = 14, /* refused: emergency stop engaged */
    BL_ERR_INTERNAL = 15,
} bl_wire_err_t;

const char* bl_wire_err_str(uint16_t e);
const char* bl_msg_type_str(uint8_t t);

#ifdef __cplusplus
}
#endif
