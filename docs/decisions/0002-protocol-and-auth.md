# ADR 0002 — Versioned wire protocol and connection-level authentication

Status: Accepted — host-verified, hardware-unverified
Date: 2026-11-23

## Context

BruceLink carries control traffic between two devices over a lossy, MTU-limited
link. We need framing, versioning, request/response correlation, error
reporting, fragmentation, and — because the link is wireless and the StickS3 is
remote — authentication and replay protection. Per the project owner's
direction, authentication is at the **connection** level (one authenticated
Flipper↔StickS3 session), **not** a separate token per Bruce function.

## Decision

### Frame (`shared/brucelink_core/src/bl_frame.*`)

```
offset size field
0      2    magic "BL"
2      1    wire version (1)
3      1    message type
4      1    flags (RESPONSE, ERROR, EVENT)
5      1    reserved (0)
6      4    seq (u32 LE) — per-session monotonic counter
10     2    payload_len (u16 LE, ≤ 512)
12     N    payload
12+N   16   MAC = HMAC-SHA256(dir_key, header‖payload)[:16]
```

Handshake frames carry a zero MAC (they authenticate via embedded confirm
tags); every post-handshake frame is MAC'd and counter-checked.

### Authenticated session (`bl_session.*`)

A provisioned 32-byte **pairing key (PSK)** plus ephemeral X25519 give mutual
authentication **and** forward secrecy:

```
C→P HELLO     proto_min,proto_max, eph_pub_C, nonce_C
P→C HELLO_ACK proto_sel, eph_pub_P, nonce_P, confirm_P = HMAC(PSK,"BLcP"‖transcript)
C→P AUTH      confirm_C = HMAC(PSK,"BLcC"‖transcript)
P→C AUTH_OK   (first MAC'd frame; establishes the session)

transcript = proto_sel ‖ eph_pub_C ‖ nonce_C ‖ eph_pub_P ‖ nonce_P
master     = HKDF-Extract(salt = nonce_C‖nonce_P, ikm = X25519(C,P) ‖ PSK)
key_c2p    = HKDF-Expand(master, "brucelink v1 c2p"‖proto, 32)
key_p2c    = HKDF-Expand(master, "brucelink v1 p2c"‖proto, 32)
```

- **Mutual auth:** each confirm tag is keyed by the PSK over the full
  transcript; a wrong PSK or any transcript tampering fails `bl_session_*`.
- **Forward secrecy:** session keys derive from an ephemeral ECDH; the long-term
  PSK compromise does not retro-decrypt past captured sessions.
- **Replay/reorder:** per-direction keys + strictly increasing `seq`
  (`bl_session_open` rejects `seq ≤ rx_highest`).
- **Version negotiation:** `pick_proto` selects the highest common app proto;
  no overlap fails closed.

Session "central" (initiator, sends HELLO) = the **Flipper**; "peripheral"
(responder) = the **StickS3**. This is independent of the BLE role (ADR 0001).

### Crypto (`bl_crypto.*`)

Self-contained SHA-256, HMAC, HKDF (RFC 5869) and X25519 (RFC 7748), so the
Flipper and the ESP32 run byte-identical code. All are verified on the host
against the published RFC/NIST vectors (`tests/c/test_crypto.c`). On the Flipper,
`furi_hal_random_fill_buf` seeds nonces/ephemerals; on the ESP32,
`esp_fill_random`.

### Fragmentation / serial framing

- `bl_reasm`: splits a frame into ≤ MTU chunks (2-byte frag header), ordered
  reassembly, gap detection. One chunk per BLE GATT write/notify.
- `bl_cobs`: COBS framing for a byte-stream (serial) transport, with a 0x00
  delimiter for resynchronisation. Not needed on BLE.

### Cancellation / connection-loss / status (project requirement)

- `BL_MSG_ESTOP` (0x7E) aborts active ops and captures.
- Link loss → `bl_safety_on_link_loss` (fail-safe e-stop) + `bl_capture_stop`
  (ADR 0003).
- `BL_MSG_STATUS` reports live capture counts + gate state.

## Why not a token per function

The connection is already mutually authenticated. A per-operation token would
duplicate that guarantee and complicate the bridge for no security gain. Active
operations are instead gated by a single session-level enable carried over the
authenticated channel (ADR 0003).

## Verification

Host (`tests/c`, ASan+UBSan, cppcheck): RFC known-answer tests for SHA-256,
HMAC (incl. keys longer than a block), HKDF (incl. multi-block output) and
X25519 (incl. all seven low-order points); header and payload tamper, replay,
out-of-order seq, counter end, fragmentation, COBS resync and a fault-injecting
simulated transport. A downgrade (a rewritten HELLO range) fails the confirm
tags (fixed 2026-11-23; before that only `proto_sel` was in the transcript). The
Python reference (`tests/test_protocol.py`) recomputes the transcript, both
confirm tags and both direction keys from the C handshake and checks the MACs
with them, so a second implementation agrees on the handshake as well as the
wire bytes. On hardware the link, pairing and a paired reconnect worked
(2026-11-23, owner's devices; `docs/TEST_RESULTS.md`).
