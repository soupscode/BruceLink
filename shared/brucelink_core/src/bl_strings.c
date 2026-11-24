/*
 * BruceLink shared core — human-readable code strings.
 * SPDX-License-Identifier: MIT
 */
#include "bl_proto.h"
#include "bl_types.h"

const char* bl_result_str(int r) {
    switch(r) {
    case BL_OK: return "ok";
    case BL_E_ARG: return "bad argument";
    case BL_E_SHORT: return "input truncated";
    case BL_E_OVERFLOW: return "output buffer too small";
    case BL_E_FORMAT: return "malformed input";
    case BL_E_VERSION: return "unsupported wire version";
    case BL_E_MAC: return "authentication tag mismatch";
    case BL_E_REPLAY: return "replay / non-increasing counter";
    case BL_E_STATE: return "invalid state for message";
    case BL_E_CRC: return "crc mismatch";
    case BL_E_TOO_LARGE: return "exceeds protocol limits";
    case BL_E_AUTH: return "handshake authentication failed";
    case BL_E_CRYPTO: return "crypto failure";
    case BL_E_SEQ: return "fragment sequence error";
    case BL_E_LOCKED: return "locked out";
    case BL_E_DENIED: return "operation denied";
    case BL_E_BUSY: return "busy";
    case BL_E_NO_RESOURCE: return "out of resource";
    default: return "unknown error";
    }
}

const char* bl_wire_err_str(uint16_t e) {
    switch(e) {
    case BL_ERR_NONE: return "none";
    case BL_ERR_BAD_FRAME: return "bad frame";
    case BL_ERR_UNSUPPORTED_VERSION: return "unsupported version";
    case BL_ERR_NOT_AUTHENTICATED: return "not authenticated";
    case BL_ERR_UNKNOWN_TYPE: return "unknown message type";
    case BL_ERR_BAD_ARG: return "bad argument";
    case BL_ERR_UNKNOWN_CAPABILITY: return "unknown capability";
    case BL_ERR_CAPABILITY_DISABLED: return "capability disabled";
    case BL_ERR_NOT_AUTHORIZED: return "not authorized";
    case BL_ERR_TARGET_NOT_ALLOWED: return "target not on allowlist";
    case BL_ERR_RATE_LIMITED: return "rate limited";
    case BL_ERR_BUSY: return "busy";
    case BL_ERR_NO_RESOURCE: return "no resource";
    case BL_ERR_NOT_FOUND: return "not found";
    case BL_ERR_ESTOPPED: return "emergency stop engaged";
    case BL_ERR_INTERNAL: return "internal error";
    case BL_ERR_USE_JOB: return "runs as a job";
    default: return "unknown wire error";
    }
}

const char* bl_msg_type_str(uint8_t t) {
    switch(t) {
    case BL_MSG_HELLO: return "HELLO";
    case BL_MSG_HELLO_ACK: return "HELLO_ACK";
    case BL_MSG_AUTH: return "AUTH";
    case BL_MSG_AUTH_OK: return "AUTH_OK";
    case BL_MSG_PAIR_COMMIT: return "PAIR_COMMIT";
    case BL_MSG_PAIR_KEY: return "PAIR_KEY";
    case BL_MSG_PAIR_REVEAL: return "PAIR_REVEAL";
    case BL_MSG_PAIR_CONFIRM: return "PAIR_CONFIRM";
    case BL_MSG_PAIR_ABORT: return "PAIR_ABORT";
    case BL_MSG_PING: return "PING";
    case BL_MSG_PONG: return "PONG";
    case BL_MSG_BYE: return "BYE";
    case BL_MSG_DEVICE_INFO_REQ: return "DEVICE_INFO_REQ";
    case BL_MSG_DEVICE_INFO: return "DEVICE_INFO";
    case BL_MSG_STATUS_REQ: return "STATUS_REQ";
    case BL_MSG_STATUS: return "STATUS";
    case BL_MSG_CAPS_REQ: return "CAPS_REQ";
    case BL_MSG_CAPS: return "CAPS";
    case BL_MSG_CMD_REQ: return "CMD_REQ";
    case BL_MSG_CMD_RESP: return "CMD_RESP";
    case BL_MSG_JOB_START: return "JOB_START";
    case BL_MSG_JOB_ACCEPTED: return "JOB_ACCEPTED";
    case BL_MSG_JOB_RESULT_REQ: return "JOB_RESULT_REQ";
    case BL_MSG_JOB_RESULT: return "JOB_RESULT";
    case BL_MSG_CAP_START: return "CAP_START";
    case BL_MSG_CAP_STOP: return "CAP_STOP";
    case BL_MSG_CAP_STATUS: return "CAP_STATUS";
    case BL_MSG_CAP_LIST: return "CAP_LIST";
    case BL_MSG_CAP_FILE_INFO: return "CAP_FILE_INFO";
    case BL_MSG_CAP_XFER_BEGIN: return "CAP_XFER_BEGIN";
    case BL_MSG_CAP_XFER_DATA: return "CAP_XFER_DATA";
    case BL_MSG_CAP_XFER_END: return "CAP_XFER_END";
    case BL_MSG_CAP_DELETE: return "CAP_DELETE";
    case BL_MSG_ESTOP: return "ESTOP";
    case BL_MSG_ERROR: return "ERROR";
    default: return "?";
    }
}
