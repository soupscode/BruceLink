# BruceLink wire protocol v1

Transport-independent protocol between the Flipper (session initiator) and the
StickS3 bridge (responder). Reference implementation: `shared/brucelink_core`
(C99) and `shared/brucelink_protocol.py` (Python). This supersedes the v0
experimental sketch. Multi-byte integers are **little-endian**.

## Layers

```
application messages (this doc)
  └─ authenticated session   bl_session : handshake, per-frame MAC, replay
       └─ frame codec         bl_frame  : header + payload + MAC tag
            └─ fragmentation   bl_reasm (BLE)  /  COBS bl_cobs (serial)
                 └─ transport  BLE GATT write/notify, or UART byte stream
```

## Frame

| off | size | field |
|----:|-----:|-------|
| 0 | 2 | magic `"BL"` (0x42 0x4C) |
| 2 | 1 | wire version = 1 |
| 3 | 1 | message type |
| 4 | 1 | flags: `0x01` RESPONSE, `0x02` ERROR, `0x04` EVENT |
| 5 | 1 | reserved (0) |
| 6 | 4 | seq (u32) — per-session monotonic counter |
| 10 | 2 | payload length (u16, ≤ 512) |
| 12 | N | payload |
| 12+N | 16 | MAC = HMAC-SHA256(dir_key, header‖payload)[:16] |

Max frame = 12 + 512 + 16 = **540 bytes**. Handshake frames carry a 16-byte
zero MAC (they authenticate via embedded confirm tags).

## Session establishment

See ADR 0002 for the full construction. Four messages:

| # | dir | type | payload |
|---|-----|------|---------|
| 1 | C→P | `HELLO` 0x01 | proto_min, proto_max, eph_pub_C(32), nonce_C(16) |
| 2 | P→C | `HELLO_ACK` 0x02 | proto_sel, eph_pub_P(32), nonce_P(16), confirm_P(32) |
| 3 | C→P | `AUTH` 0x03 | confirm_C(32) |
| 4 | P→C | `AUTH_OK` 0x04 | (first MAC'd frame) |

C = Flipper (session central/initiator), P = StickS3 (session peripheral). Keys:
HKDF-SHA256 over `X25519(eph_C,eph_P) ‖ PSK`, salted by both nonces; separate
c2p/p2c keys. Replay prevented by the strictly increasing `seq`.

The PSK comes from pairing (below). The public demo key (`bl_demo_key.h`) is
used as a key only by host tests and the ESP32 compile check. The StickS3 knows
it only to refuse it: its key store rejects it, and a bridge holding it refuses
every active op. The Flipper app does not contain it.

## Pairing

Run instead of a session when the user puts both devices into pairing mode.
Gives both a fresh PSK; a 6-digit code shown on both screens protects against a
man in the middle. Full construction and rationale: ADR 0005. Zero MAC field,
`seq` 0, one attempt per link, 90 s limit.

| # | dir | type | payload |
|---|-----|------|---------|
| 1 | C→P | `PAIR_COMMIT` 0x08 | ver(1)=1, commit(32), name_len(1), name(≤16, printable ASCII) |
| 2 | P→C | `PAIR_KEY` 0x09 | ver(1)=1, pub_P(32), nonce_P(16), name_len(1), name(≤16) |
| 3 | C→P | `PAIR_REVEAL` 0x0A | pub_C(32), nonce_C(16) |
| 4 | C↔P | `PAIR_CONFIRM` 0x0B | tag(32), sent by each side after its user accepts |
| – | C↔P | `PAIR_ABORT` 0x0C | reason(1): 1 rejected, 2 cancelled, 3 timeout, 4 check failed |

```
commit = SHA256("BLpair1 commit" ‖ 0x01 ‖ pub_C ‖ nonce_C)
T      = SHA256("BLpair1 transcript" ‖ 0x01 ‖ lenC ‖ name_C ‖ pub_C ‖ nonce_C
                                          ‖ lenP ‖ name_P ‖ pub_P ‖ nonce_P)
code   = LE32(SHA256("BLpair1 code" ‖ T)[0..3]) mod 1 000 000
psk    = HKDF-SHA256(salt = nonce_C ‖ nonce_P, ikm = X25519, info = "BLpair1 psk" ‖ T, 32)
tag_C  = HMAC-SHA256(psk, "BLpair1 confirm I" ‖ T);   tag_P: "... confirm R"
key id = SHA256("BLpair1 key id" ‖ psk)[0..1]          (display only)
```

## Message types

| Type | Name | Dir | Payload |
|------|------|-----|---------|
| 0x05 | PING / 0x06 PONG | C↔P | echo bytes |
| 0x07 | BYE | C→P | — (graceful close) |
| 0x08–0x0C | PAIR_* | C↔P | pairing only (above) |
| 0x10/0x11 | DEVICE_INFO_REQ / DEVICE_INFO | C→P/P→C | text summary |
| 0x12/0x13 | STATUS_REQ / STATUS | — | capture + gate status text |
| 0x14/0x15 | CAPS_REQ / CAPS | — | capability registry (below) |
| 0x20/0x21 | CMD_REQ / CMD_RESP | — | `[u16 cap_id][args]` / result text |
| 0x22/0x23 | JOB_START / JOB_ACCEPTED | C→P/P→C | jobs (below) |
| 0x24/0x25 | JOB_RESULT_REQ / JOB_RESULT | C→P/P→C | jobs (below) |
| 0x40 | CAP_START | C→P | `[u8 mode][u8 channel]` |
| 0x41 | CAP_STOP | C→P | — |
| 0x42 | CAP_STATUS | C→P | — |
| 0x43 | CAP_LIST | C→P | — (pcap file listing) |
| 0x44 | CAP_FILE_INFO | P→C | name, size, CRC-32 |
| 0x45/46/47 | CAP_XFER_BEGIN / DATA / END | — | resumable transfer w/ offset |
| 0x48 | CAP_DELETE | C→P | file id |
| 0x7E | ESTOP | C→P | — (abort active ops + capture; also auto on link loss) |
| 0x7F | ERROR | P→C | `[u16 wire_err]` |

### Capability registry (CAPS payload)

`u8 count`, then per item: `u16 id`, `u8 op_class` (0 passive / 1 active-RF),
`u8 flags`, `u8 min_proto`, `u8 name_len`, `name[name_len]`.

Flags: `0x01` ACTIVE_RF, `0x02` REQUIRES_EXT_HW, `0x04` HW_UNVERIFIED,
`0x08` REQUIRES_TARGET, `0x10` JOB (runs as a job, below). Canonical ids in
`bl_capability.h`.

### Jobs (ADR 0006)

A capability flagged JOB needs the StickS3's radio memory to itself, so it
never runs next to the link: the StickS3 accepts it, restarts, runs it with
BLE off, restarts again and brings the link back. The result is fetched on the
next session. Invoking it with `CMD_REQ` returns `use_job`.

| Message | Payload (little-endian) |
|---|---|
| `JOB_START` | `u16 cap_id`, `u16 max_s` (0 = the capability's default), `args[0..32]` |
| `JOB_ACCEPTED` | `u32 job_id` (never 0), `u16 max_s` (the limit that applies) |
| `JOB_RESULT_REQ` | `u32 job_id` |
| `JOB_RESULT` | `u32 job_id`, `u8 outcome`, `text[0..480]` |

Outcomes: 1 done, 2 failed (text says why), 3 stopped with a button on the
StickS3, 4 time limit reached, 5 interrupted (the StickS3 reset during the
job; it is never run again).

- `JOB_START` errors, in order: `bad_arg` (malformed), `unknown_capability`,
  `bad_arg` (not a job capability), `capability_disabled` (active jobs, not
  yet allowed), `not_authorized` (bridge holds the demo key), `estopped`,
  `bad_arg` (`max_s` over the capability's limit, or arguments it does not
  take), `busy` (a job was already accepted and the restart is pending).
- After `JOB_ACCEPTED` the StickS3 releases the link within about 0.5 s. The
  Flipper must not fetch on that session; it fetches after the link has
  dropped and a new session is up, and gives up at `max_s` + 60 s.
- `JOB_RESULT_REQ` errors: `bad_arg`, `not_found` (no such job: never
  accepted, replaced by a newer one, or lost with power), `busy` (not
  finished). A result can be fetched more than once.
- Limits per capability (bridge): `wifi.scan` default 20 s, at most 60 s, no
  arguments.

### Generalized command adapter

`CMD_REQ` payload = `[u16 cap_id][args]`. The bridge maps `cap_id` to a backend
call, so new Bruce features need only a registry entry + a backend method, no
protocol change. Reserved control id `0xFF01` = enable/disable active ops
(`[u8 enable]`). Active capabilities (`ACTIVE_RF`) require the operator to have
enabled active ops over the authenticated session and pass the remote safety
gate (ADR 0003); `REQUIRES_TARGET` ones must name a BSSID (no broadcast).

### Active-op args

`CMD_REQ` for an active capability: `[u16 cap_id][u8 channel][u8 bssid[6]]`.

- `channel` must be 1–14 and `bssid` a **specific unicast** address (I/G bit
  clear, not all-zero); broadcast/multicast → `bad_arg`.
- Checked in order: e-stop (`estopped`) → active ops enabled
  (`not_authorized`) → args (`bad_arg`) → frame budget (`rate_limited`) →
  operator scope (`target_not_allowed`).
- **Frame budget:** the bridge reserves `min(16, frames left in the rate
  window)` and passes it to Bruce as a hard cap on transmitted frames; unused
  frames are refunded. A backend that reports more than its cap trips the
  e-stop (`internal`). The window default is 64 frames / 1000 ms.

## Emergency stop and session lifetime

- `ESTOP` latches for the **rest of the session**: every radio operation
  (`JOB_START`, `CAP_START`, active ops) is refused with `estopped`, and any
  capture is stopped and flushed. Read-only queries (`PING`, `DEVICE_INFO`,
  `CAPS`, `STATUS`) and `CAP_STOP` stay available.
- A **new authenticated session** starts with the gate reset: e-stop cleared and
  active ops disabled until the operator re-enables them. The rate window is
  **not** reset: it is device transmit history and only elapsed time expires it.
- Link loss = automatic e-stop + capture stop (fail-safe).
- **Handshake deadline:** after the BLE link comes up, the session must be
  established within **10 s**, or the StickS3 drops the link. A forged proof
  drops it immediately. Either way, that peer address is skipped for 30 s.
  The Flipper must send `HELLO` as soon as the StickS3 enables TX
  notifications (ADR 0001 §6).

## Capture

`CAP_START [u8 mode][u8 channel]`: mode 0 handshake-only, 1 raw (needs SD;
`no_resource` on a bare StickS3), 2 deauth-pcap; channel 1–14. Errors:
`estopped`, `capability_disabled` (build has no capture backend), `bad_arg`,
`busy`, `no_resource`. If the backend fails mid-capture (e.g. LittleFS full) the
bridge stops and flushes it immediately; `STATUS` then reports `capture:error`
with the cause in `err:` until `CAP_STOP` or a new start.

`STATUS` text: `capture:<idle|running|error> pkts:N eapol:N hs:N err:<code>
estop:<0|1> active:<0|1>`.

## Wire error codes (ERROR payload, u16)

0 none · 1 bad_frame · 2 unsupported_version · 3 not_authenticated ·
4 unknown_type · 5 bad_arg · 6 unknown_capability · 7 capability_disabled ·
8 not_authorized · 9 target_not_allowed · 10 rate_limited · 11 busy ·
12 no_resource · 13 not_found · 14 estopped · 15 internal · 16 use_job (this
capability runs as a job).

## Fragmentation (BLE)

Each chunk: `[u8 flags: 0x01 FIRST, 0x02 LAST][u8 index][data…]`, sized to the
negotiated ATT MTU − 3. Reassembler requires FIRST, contiguous indices, and
bounds the total to 540 bytes (`bl_reasm`).

## Serial framing (COBS)

For a UART byte stream: COBS-encode each frame, append `0x00`. A reader resyncs
on the delimiter (`bl_cobs`). Not used on BLE (GATT writes are message-framed).
