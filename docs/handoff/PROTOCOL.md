# BruceLink Wire + Session Protocol Reference

Authoritative reference for BruceLink wire protocol **v1**, drawn from the code
in `shared/brucelink_core/src/` (the single source of truth — both devices and
the host tests compile it byte-identically). Where this doc and a prose file
disagree, the code wins; discrepancies are flagged inline.

Primary sources: `shared/brucelink_core/src/{bl_proto.h, bl_types.h, bl_frame.h,
bl_session.{h,c}, bl_client.h, bl_pair.{h,c}, bl_job.h, bl_files.h, bl_reasm.h,
bl_cobs.h, bl_safety.h, bl_capability.h, bl_crypto.h, bl_pcap.h, bl_uuid.h}`,
plus `docs/decisions/0002-protocol-and-auth.md`, `docs/decisions/0005-pairing.md`,
and `shared/protocol.md`. Companion docs: `CODEBASE_MAP.md` (where each symbol
lives), `DECISION_HISTORY.md` (ADR rationale), `NATIVE_BRUCE_INVENTORY.md` /
`INTEGRATION_INTERFACES.md` (upstream Bruce).

Verification tags: **SW-VERIFIED** = exercised by host tests / the Python
interop at `448b798` (core 3681 + bridge 448 checks + 31 Python tests, 0 failed
— `../TEST_RESULTS.md`, `CURRENT_STATE.md`); **HW-OWNER** = ran on the owner's
devices (2026-11-23), not independently verified; **DESIGN** = specified, code
not shipping/exercised.

> **Confidentiality status (read first).** Frames are **AUTHENTICATED but NOT
> ENCRYPTED** until milestone M4.6 (`bl_files.h`, ADR 0005 §Consequences, ADR
> 0001 amendment). A nearby BLE sniffer can read every payload, including a
> downloaded handshake capture. The pairing key authenticates and anti-replays
> frames; it does not encrypt them. State this wherever BruceLink status is
> reported.

Multi-byte integers are **little-endian** everywhere.

---

## 1. Layering

```
application messages (this doc)
  └─ authenticated session   bl_session : handshake, per-frame MAC, replay counters
       └─ frame codec         bl_frame  : 12-B header + payload + 16-B MAC tag
            └─ fragmentation   bl_reasm (BLE)  /  COBS  bl_cobs (serial byte stream)
                 └─ transport  BLE GATT write/notify (the only shipping transport)
```

Pairing (`bl_pair`) runs **instead of** a session when both devices are put in
pairing mode: same frame codec, but a zero MAC and no session keys (§6).

---

## 2. Frame format (`bl_frame.h`, `bl_proto.h`, `bl_types.h`)

A logical message is `header(12) || payload(0..512) || mac(16)`.

| Offset (macro) | Size | Field |
|---|---|---|
| `BL_OFF_MAGIC0`=0 | 1 | magic `'B'` (`BL_MAGIC0`, 0x42) |
| `BL_OFF_MAGIC1`=1 | 1 | magic `'L'` (`BL_MAGIC1`, 0x4C) |
| `BL_OFF_VERSION`=2 | 1 | wire version = 1 (`BL_WIRE_VERSION`) |
| `BL_OFF_TYPE`=3 | 1 | message type (`bl_msg_type_t`) |
| `BL_OFF_FLAGS`=4 | 1 | flags (below) |
| `BL_OFF_TAG`=5 | 1 | **request tag** — non-zero in a request, echoed in its response; 0 elsewhere |
| `BL_OFF_SEQ`=6 | 4 | `seq` (u32 LE): per-session monotonic counter |
| `BL_OFF_LEN`=10 | 2 | `payload_len` (u16 LE, ≤ `BL_MAX_PAYLOAD`=512) |
| 12 | N | payload |
| 12+N | 16 | MAC (`BL_MAC_TAG_LEN`=16) |

Constants: `BL_HDR_LEN`=12, `BL_MAC_LEN`=16, `BL_MAX_PAYLOAD`=512, so
`BL_MAX_MSG` = max frame = 12 + 512 + 16 = **540 bytes**.

**Flags** (`bl_proto.h`): `BL_FLAG_NONE`=0x00, `BL_FLAG_RESPONSE`=0x01,
`BL_FLAG_ERROR`=0x02, `BL_FLAG_EVENT`=0x04 (unsolicited notification).

**MAC.** `MAC = HMAC-SHA256(dir_key, header || payload)` truncated to 16 bytes.
The whole 12-byte header — including the tag at byte 5 — is covered.
`bl_frame_decode` verifies it in **constant time** (`bl_ct_equal`) and returns
`BL_E_MAC` on mismatch. The direction key is the sender's egress key
(`tx_key`/`rx_key`, §4).

**Handshake and pairing frames carry a 16-byte ZERO MAC** — there is no session
key yet; their integrity comes from the embedded confirm tags and (pairing) the
user's code comparison. Every post-handshake data frame is MAC'd and
counter-checked. `bl_frame_encode` appends 16 zero bytes when `mac_key` is NULL,
otherwise the tag.

> **Doc discrepancy.** ADR 0002's frame table calls byte 5 "reserved (0)". The
> code (`BL_OFF_TAG`, `bl_frame_t.tag`) and `shared/protocol.md` define it as
> the request tag (§5). The code is authoritative; ADR 0002's table is stale on
> this one field.

SW-VERIFIED: encode/decode, header+payload tamper, truncation, oversize
(`tests/c/test_protocol_core.c`), and byte-for-byte against the Python reference
(`tests/test_protocol.py` via `tools/bl_vectors.c`).

---

## 3. Message types (`bl_proto.h` `bl_msg_type_t`)

Ranges: 0x00–0x0F control; 0x10–0x3F info; 0x40–0x6F capture; 0x70–0x7E
reserved; 0x7F error.

| Type | Name | Dir | Purpose |
|---|---|---|---|
| 0x01 | `BL_MSG_HELLO` | C→P | handshake: proto range + eph pub + nonce |
| 0x02 | `BL_MSG_HELLO_ACK` | P→C | handshake: proto sel + eph pub + nonce + confirm_P |
| 0x03 | `BL_MSG_AUTH` | C→P | handshake: confirm_C |
| 0x04 | `BL_MSG_AUTH_OK` | P→C | first MAC'd frame; session established |
| 0x05 / 0x06 | `BL_MSG_PING` / `BL_MSG_PONG` | C↔P | echo |
| 0x07 | `BL_MSG_BYE` | C→P | graceful teardown |
| 0x08 | `BL_MSG_PAIR_COMMIT` | I→R | pairing: commitment to initiator's key |
| 0x09 | `BL_MSG_PAIR_KEY` | R→I | pairing: responder's key + nonce + name |
| 0x0A | `BL_MSG_PAIR_REVEAL` | I→R | pairing: initiator's key + nonce |
| 0x0B | `BL_MSG_PAIR_CONFIRM` | I↔R | pairing: key confirmation after accept |
| 0x0C | `BL_MSG_PAIR_ABORT` | I↔R | pairing: reject / cancel / timeout |
| 0x10 / 0x11 | `BL_MSG_DEVICE_INFO_REQ` / `BL_MSG_DEVICE_INFO` | C→P / P→C | text summary |
| 0x12 / 0x13 | `BL_MSG_STATUS_REQ` / `BL_MSG_STATUS` | C↔P | capture + gate status text |
| 0x14 / 0x15 | `BL_MSG_CAPS_REQ` / `BL_MSG_CAPS` | C↔P | capability registry |
| 0x20 / 0x21 | `BL_MSG_CMD_REQ` / `BL_MSG_CMD_RESP` | C↔P | invoke a capability by id |
| 0x22 / 0x23 | `BL_MSG_JOB_START` / `BL_MSG_JOB_ACCEPTED` | C→P / P→C | jobs (§7) |
| 0x24 / 0x25 | `BL_MSG_JOB_RESULT_REQ` / `BL_MSG_JOB_RESULT` | C→P / P→C | jobs (§7) |
| 0x40 | `BL_MSG_CAP_START` | C→P | inline capture start (refused on StickS3 — runs as a job) |
| 0x41 | `BL_MSG_CAP_STOP` | C→P | inline capture stop |
| 0x42 | `BL_MSG_CAP_STATUS` | C→P | inline capture status |
| 0x43 | `BL_MSG_CAP_LIST` | C↔P | capture-file list page (§8) |
| 0x44 | `BL_MSG_CAP_FILE_INFO` | C↔P | file size + CRC-32 (§8) |
| 0x45 / 0x47 | `BL_MSG_CAP_XFER_BEGIN` / `BL_MSG_CAP_XFER_END` | — | reserved (reads are stateless) |
| 0x46 | `BL_MSG_CAP_XFER_DATA` | C↔P | read at an offset (§8) |
| 0x48 | `BL_MSG_CAP_DELETE` | C↔P | delete a file (§8) |
| 0x7E | `BL_MSG_ESTOP` | C→P | emergency stop (§9) |
| 0x7F | `BL_MSG_ERROR` | P→C | `[u16 wire_err]` (§10) |

C = Flipper (session **central**/initiator). P = StickS3 (session
**peripheral**/responder). These session roles are the reverse of the BLE roles
(ADR 0001: the Flipper is the BLE peripheral, the StickS3 the BLE central) —
`bl_session.h` is explicit about this.

---

## 4. Session establishment (`bl_session.{h,c}`, ADR 0002)

A provisioned 32-byte **pairing key (PSK)** plus an ephemeral **X25519** ECDH
give mutual authentication **and** forward secrecy. Four messages:

| # | Dir | Type | Payload |
|---|---|---|---|
| 1 | C→P | HELLO | `proto_min(1)`, `proto_max(1)`, `eph_pub_C(32)`, `nonce_C(16)` — 50 B |
| 2 | P→C | HELLO_ACK | `proto_sel(1)`, `eph_pub_P(32)`, `nonce_P(16)`, `confirm_P(32)` — 81 B |
| 3 | C→P | AUTH | `confirm_C(32)` |
| 4 | P→C | AUTH_OK | (empty; first MAC'd frame) |

Nonces are `BL_NONCE_LEN`=16, keys `BL_KEY_LEN`=32. On the Flipper nonces/eph
keys come from `furi_hal_random_fill_buf`; on the ESP32, `esp_fill_random`
(injected via `bl_rng_fn`; a deterministic source is injected in host tests).

### Transcript and derivation (verbatim from `bl_session.{h,c}`)

```
transcript = hello_min || hello_max || proto_sel
             || eph_pub_C || nonce_C || eph_pub_P || nonce_P

confirm_P  = HMAC-SHA256(PSK, "BLcP" || transcript)     ; label CONFIRM_P
confirm_C  = HMAC-SHA256(PSK, "BLcC" || transcript)     ; label CONFIRM_C

master     = HKDF-Extract(salt = nonce_C || nonce_P,
                          ikm  = X25519(eph_C, eph_P) || PSK)

tx/rx key  = HKDF-Expand(master, "brucelink v1 c2p" || proto_sel, 32)   ; INFO_C2P
             HKDF-Expand(master, "brucelink v1 p2c" || proto_sel, 32)   ; INFO_P2C
```

`c2p` keys central→peripheral traffic, `p2c` the reverse; each side uses the
peer's direction key to verify. `hello_min`/`hello_max` are set to
`BL_PROTO_MIN`/`BL_PROTO_MAX` by the central and recorded by the peripheral
(`bl_session.c` ~L163–209).

- **Mutual auth.** Each confirm tag is keyed by the PSK over the full
  transcript; a wrong PSK or any transcript tampering fails the tag. On a bad
  proof the session moves to `BL_SS_FAILED` and `bl_session_close` wipes all key
  material (`bl_secure_zero`); only `BL_SS_ESTABLISHED` can seal/open frames.
- **Forward secrecy.** Session keys derive from the ephemeral ECDH, so a later
  PSK compromise does not retro-decrypt past (once M4.6 lands, encrypted)
  sessions. X25519 rejects low-order points (`bl_x25519` → `BL_E_CRYPTO` on an
  all-zero result; all seven low-order points are tested).
- **Version negotiation.** `pick_proto(peer_min, peer_max)` selects the highest
  common app proto; no overlap fails closed (`BL_E_VERSION`). Today
  `BL_PROTO_MIN = BL_PROTO_MAX = 1`. Because the **full** range is in the
  transcript, a range rewritten in flight (a downgrade attempt) fails both
  confirm tags.

> **Doc discrepancy (important).** ADR 0002 shows `transcript = proto_sel || …`
> (range **not** included). That was the pre-2026-11-23 behaviour and is now a
> known downgrade gap that was fixed: the code and `shared/protocol.md` include
> `hello_min || hello_max` first. Treat `bl_session.{h,c}` as authoritative.
> Both devices must run the **same build** — a transcript change is a wire
> change.

### State machine (`bl_session_state_t`)

`BL_SS_INIT` → (central) `WAIT_HELLO_ACK` → `WAIT_AUTH_OK` → `ESTABLISHED`;
(peripheral) `WAIT_HELLO` → `WAIT_AUTH` → `ESTABLISHED`; `FAILED`/`CLOSED`
terminal. In `WAIT_AUTH_OK` only `AUTH_OK` ends the wait — other frames are
ignored, not fatal.

### Replay / reorder

Per-direction keys + a strictly increasing `seq`. `bl_session_seal`/`seal_tag`
stamp the next `tx_counter`; `bl_session_open` rejects `seq ≤ rx_highest`
(`BL_E_REPLAY`). A sealed-but-unsent frame leaves a gap, which the receiver
accepts (it only requires strict increase).

SW-VERIFIED: the Python reference independently recomputes the transcript, both
confirm tags, both direction keys and the MACs from the C handshake
(`tests/test_protocol.py`); crypto primitives are checked against RFC/NIST
vectors (`tests/c/test_crypto.c`). HW-OWNER: link + handshake on the owner's
devices (2026-11-23).

---

## 5. Request/response tagging (`bl_frame_t.tag`, `bl_client.{h,c}`)

The central puts a non-zero tag (1–255, wrapping, never 0) in header byte 5 of
each request; the peripheral copies it into the response. The tag is under the
MAC. The client (`bl_client_t`) tracks exactly **one** outstanding request
(`req_pending`, `req_tag`) and surfaces a response only if its tag matches the
pending request's — a reply that arrives after its request timed out is dropped
(`stale_responses++`) instead of being taken as the answer to the next request
(fixed in M4.7). Events and all handshake/pairing frames carry tag 0.

Timeouts: handshake must finish within `BL_CLIENT_HS_TIMEOUT_MS` (5000 ms) of
link-up or the client reports `BL_CL_EVT_HANDSHAKE_TIMEOUT` once and the caller
drops the link (no in-place retransmit — a clean new link instead). A request
must be answered within `BL_CLIENT_REQ_TIMEOUT_MS` (10000 ms, long enough for a
scan job to run) or `BL_CL_EVT_REQUEST_TIMEOUT` frees the slot; the session
stays up. SW-VERIFIED (`tests/c/test_client.c`, `test_integration.c`).

---

## 6. Pairing (`bl_pair.{h,c}`, ADR 0005)

Gives two devices that share nothing a fresh 32-byte PSK, protected against a
man-in-the-middle by the user comparing a 6-digit code on both screens (like
Bluetooth numeric comparison). Runs only when both devices are put in pairing
mode. Frames carry a **zero MAC**, `seq`=0; one attempt per BLE link;
`BL_PAIR_TIMEOUT_MS`=90 000. I = initiator (Flipper), R = responder (StickS3).

| # | Dir | Type | Payload |
|---|---|---|---|
| 1 | I→R | PAIR_COMMIT | `ver(1)=1`, `commit(32)`, `name_len(1)`, `name_I(≤16 printable ASCII)` |
| 2 | R→I | PAIR_KEY | `ver(1)=1`, `pk_R(32)`, `n_R(16)`, `name_len(1)`, `name_R(≤16)` |
| 3 | I→R | PAIR_REVEAL | `pk_I(32)`, `n_I(16)` (R checks the commit; I checks pk_R usable) |
| 4 | I↔R | PAIR_CONFIRM | `tag(32)` — each side sends after its user accepts |
| – | I↔R | PAIR_ABORT | `reason(1)` — see `bl_pair_abort_t` |

```
commit = SHA256("BLpair1 commit" || 0x01 || pk_I || n_I)
T      = SHA256("BLpair1 transcript" || 0x01 || lenI || name_I || pk_I || n_I
                                             || lenR || name_R || pk_R || n_R)
code   = LE32(SHA256("BLpair1 code" || T)[0..3]) mod 1 000 000     ; BL_PAIR_CODE_MOD
psk    = HKDF-SHA256(salt = n_I || n_R, ikm = X25519(sk, pk_peer),
                     info = "BLpair1 psk" || T, 32)
tag_I  = HMAC-SHA256(psk, "BLpair1 confirm I" || T)
tag_R  = HMAC-SHA256(psk, "BLpair1 confirm R" || T)
key id = SHA256("BLpair1 key id" || psk)[0..1]        ; display only, BL_PAIR_KEY_ID_LEN=2
```

- **Commitment/confirm flow.** I commits to its key before seeing R's, and R
  reveals before seeing I's, so a MITM must fix its own key on one side before
  it can know that side's code — codes agree with probability 10⁻⁶ per attempt,
  and every attempt needs both users to accept. Confirm tags prove each side
  holds the same key and are sent only after the local accept; a side that
  accepted keeps nothing unless the peer's tag verifies.
- **Abort reasons** (`bl_pair_abort_t`): 1 rejected (codes differ), 2 cancelled,
  3 timeout, 4 commit/key check failed.
- **States** (`bl_pair_state_t`): `IDLE → WAIT_COMMIT/WAIT_KEY → WAIT_REVEAL →
  COMPARE → WAIT_PEER → DONE`; `FAILED`. Failure classes in `bl_pair_fail_t`.
- **Names** are shown on the peer's screen, covered by the code, ≤16 printable
  ASCII (`BL_PAIR_NAME_MAX`=16), non-printable → `?`.
- **Modes are separate.** A StickS3 in pairing mode never authenticates a
  session and vice-versa; a non-pairing Flipper reaching a pairing StickS3 is
  dropped + cooled down.

### Key ids and storage

The 2-byte key id is for display only (never the key). Key storage
(`CODEBASE_MAP.md` §3/§5):

| Device | Location | Protection |
|---|---|---|
| Flipper | `apps_data/brucelink/pairing.key` (SD) | AES-CBC under the device-unique enclave key (like stock U2F); key id checked on load (no MAC — a damaged file passes ~1/65536 then fails the handshake). Written temp-then-renamed. |
| StickS3 | NVS (`Preferences`, namespace `brucelink`) | **Never** `bruce.conf` (printed to serial) or LittleFS (readable via Bruce's file browser/web UI). Flash encryption is off → USB physical access can read it. |

### Public demo key

`bl_demo_key.h`'s `BRUCELINK_DEMO_PSK` is **public** and provides no security. It
is used **as a key** only by host tests and the ESP32 compile check. The StickS3
carries it only to refuse it (its key store rejects it; a bridge holding it
refuses every active op/job/capture). The Flipper app does not contain it at all
(`tests/test_demo_key.py` checks sources + the built `.fap`). **Never ship the
demo key as a key.** (No key bytes are reproduced in this doc; key ids are
fine.)

SW-VERIFIED: `tests/c/test_pair.c` (13 tests incl. MITM + per-field tampering +
6 broken code variants), bridge pairing tests, Python recomputation of every
derived value. HW-OWNER: pairing completed and the paired key reconnected after
a StickS3 reboot (2026-11-23); key-id display / Forget not yet checked.

---

## 7. Jobs (`bl_job.{h,c}`, ADR 0006)

A capability flagged `BL_CAP_F_JOB` needs the StickS3's single 2.4 GHz radio to
itself and cannot run next to the BLE link (Wi-Fi + BLE do not fit — ADR 0006,
`MEMORY_OPTIMIZATION.md`). The StickS3 accepts the job, drops the link,
restarts with BLE off, runs the tool, restarts again and brings the link back;
the result is fetched on the next session.

| Message | Payload (LE) |
|---|---|
| JOB_START (0x22) | `u16 cap_id`, `u16 max_s` (0 = capability default), `args[0..32]` (`BL_JOB_ARGS_MAX`=32) |
| JOB_ACCEPTED (0x23) | `u32 job_id` (never 0), `u16 max_s` (the limit that applies) |
| JOB_RESULT_REQ (0x24) | `u32 job_id` |
| JOB_RESULT (0x25) | `u32 job_id`, `u8 outcome`, `text[0..480]` (`BL_JOB_RESULT_MAX`=480) |

Codecs: `bl_job_encode_start/parse_start`, `…accepted`, `…result_req`,
`…result`. `BL_JOB_MAX_S`=300 is the upper bound for any job's limit.

### Job record and "never run twice" (`bl_job_record_t`, `bl_job_on_boot`)

The record lives in RTC memory not initialised at boot (`RTC_NOINIT` on the
StickS3): survives restarts, panics and watchdog resets, **not** a power-off.
Fixed layout, no padding; validated by magic (`BL_JOB_MAGIC` "BLJB"), version,
CRC and bounds (`bl_job_record_valid`).

States (`bl_job_state_t`): `NONE → PENDING → RUNNING → FINISHED`.
`bl_job_on_boot` is called once per boot **before anything else touches the
record** and records the transition, so a reset at any point never re-runs a
job:

```
invalid                    -> cleared, NONE
PENDING                    -> RUNNING, returns RUN
RUNNING (reset mid-job)    -> FINISHED/INTERRUPTED  (never re-run)
FINISHED with resume_link  -> resume_link cleared, returns LINK
FINISHED without           -> NONE (result kept for fetching)
```

Outcomes (`bl_job_outcome_t`): 1 DONE, 2 FAILED, 3 STOPPED (button on the
stick), 4 TIME_LIMIT, 5 INTERRUPTED (reset during the job; not re-run).

### Cancel-before-commit

After JOB_ACCEPTED, the bridge keeps serving the link until it is about to
restart. An **e-stop or link loss before `BruceLinkBridge::commitJob()`** cancels
the job (the record is cleared and the job never runs — `cancelPendingJob`,
`jobPending`). After commit, a link loss is the restart itself and cancels
nothing.

### Flipper-side tracker (`bl_jobc_t`)

States `IDLE → STARTING → RUNNING → FETCHING → DONE`/`FAILED`
(`bl_jobc_state_t`). It follows one job through the link drop, waits for a new
session, then sends JOB_RESULT_REQ (`bl_jobc_want_fetch`/`fetch`) and gives up
at `max_s` + `BL_JOB_RESTART_ALLOWANCE_MS` (60 000 ms, an estimate until
measured). Failure classes in `bl_jobc_fail_t` (`REFUSED`, `NO_ANSWER`,
`TIMEOUT`, `LOST` = the StickS3 no longer has the job, `PROTOCOL`).

### Active jobs are refused

An active-RF capability run as a job is **refused** (`capability_disabled`)
until ADR 0006's active-job rules (gate at acceptance, duration cap, stop button
on the stick, no re-run) are implemented and tested (`CLAUDE.md` security
rules). Shipping jobs today are passive only: `wifi.scan` and
`wifi.sniff.handshake`.

SW-VERIFIED: `tests/c/test_job.c`, bridge job tests. HW-OWNER: a job across both
restarts (2026-11-23).

---

## 8. Capture-file transfer (`bl_files.{h,c}`)

Reading a file needs no radio, so these run on the **live link** (ADR 0006 only
moves radio work into jobs). Every message is a request (C→P) and a response
(P→C, RESPONSE flag) of the same type. `bl_pcap`'s CRC-32 guards integrity.

| Message | Request | Response |
|---|---|---|
| CAP_LIST (0x43) | `[u8 start]` | `[u8 total][u8 start][u8 count]` then `count × ([u32 size][u8 name_len][name])` |
| CAP_FILE_INFO (0x44) | `[u8 name_len][name]` | `[u32 size][u32 crc32][u8 name_len][name]` |
| CAP_XFER_DATA (0x46) | `[u32 offset][u16 max_len][u8 name_len][name]` | `[u32 offset][data]`, data ≤ max_len |
| CAP_DELETE (0x48) | `[u8 name_len][name]` | empty |

Codecs: `bl_files_encode_list_req`/`parse_list_req`, `list_begin`/`list_add`/
`parse_list`, `encode_name`/`parse_name`, `encode_info`/`parse_info`,
`encode_read_req`/`parse_read_req`, `encode_data_head`/`parse_data`.

- **Name rules** (`bl_file_name_ok`, enforced on **both** devices): bare file
  name, never a path; `[A-Za-z0-9._-]`, 1–`BL_FILE_NAME_MAX`(63) bytes, not
  starting with `.`, ending in `.pcap`. Covers what Bruce's sniffer writes
  (incl. SSIDs with dots, e.g. `HS_<MAC>_Cafe..pcap`); with no separator and no
  leading dot, nothing outside the capture dir
  (`/BrucePCAP/handshakes`) can be named.
- **Chunk size.** `BL_FILE_CHUNK_MAX`=448 data bytes per CAP_XFER_DATA reply;
  `max_len` 1–448. `BL_FILE_LIST_PAGE`=7 entries per CAP_LIST page (fits 512 B);
  `BL_FILE_LIST_MAX`=255 files.
- **Stateless reads + resume.** Every read names its offset, so a dropped link
  is resumed: re-fetch the info and, if size+CRC are unchanged, continue from
  the current offset; a changed file fails the download
  (`BL_XFER_F_CHANGED`). Tracker `bl_xferc_t`:
  `IDLE → INFO → DATA → DONE`/`FAILED`.
- **CRC-32** (IEEE 802.3, reflected, poly `0xEDB88320`; `bl_crc32_*`) of the
  whole file is checked once the last byte is in (`BL_XFER_F_CRC` on mismatch).
- **Retry cap.** `BL_XFER_RETRY_MAX`=5 consecutive retries without a byte of
  progress → `BL_XFER_F_GAVE_UP`. The Flipper writes `<name>.part` under
  `/ext/apps_data/brucelink/captures/` and renames only on success.
- **Send-buffer bound.** `BL_FILES_REQ_FRAME_MAX` = 98 B (header + longest
  request payload + MAC).
- **Errors** (§10): `bad_arg`, `not_found` (no such file / offset past end),
  `not_authorized` (bridge holds the demo key), `internal`.

SW-VERIFIED: `tests/c/test_files.c`. The file paths/UI are HW-OWNER /
DESIGN depending on the Bruce sniffer hooks (M5.2 — see `PATCH.md`).

---

## 9. Safety gate on the wire (`bl_safety.{h,c}`, ADR 0003)

Authorization = the authenticated connection (ADR 0002). There is **no
per-function auth token**. The gate adds only the safeguards remote operation
needs. Wire-relevant fields:

- **Op class** (`bl_op_class_t`): `BL_OP_PASSIVE`=0 (scans, info, capture RX,
  file ops — pass whenever not e-stopped), `BL_OP_ACTIVE`=1 (RF transmission).
- **Active-ops enable.** A single session-level switch (default **OFF**), set
  over the already-authenticated link via the reserved control capability
  `kCtrlEnableActive`=0xFF01 (`CMD_REQ` payload `[u16 0xFF01][u8 enable]`).
- **Emergency stop** (`BL_MSG_ESTOP`=0x7E): latches the gate closed for the rest
  of the session (every radio op refused with `estopped`; any capture
  stopped/flushed); read-only queries and CAP_STOP stay available. Engaged
  automatically on link loss (`bl_safety_on_link_loss`).
- **Target.** An active op must name **one specific unicast** BSSID
  (`bl_safety_bssid_is_specific`: rejects all-zero and any group/broadcast/
  multicast — I/G bit set). Never broadcast, never targetless.
- **Frame budget.** A fixed-window rate limiter (`window_ms`,
  `max_frames_per_window`; default 64 frames / 1000 ms). The bridge reserves
  `min(kMaxFramesPerActiveOp=16, remaining budget)` and passes it to Bruce as a
  hard cap; unused frames are refunded (`bl_safety_budget`/`refund`). The window
  records frames actually sent and is expired **only by elapsed time** —
  enabling/disabling active ops, e-stop reset and a new session do not clear it.
  A backend transmitting more than its cap trips the e-stop (`internal`).
- **Optional target scope** (allowlist, `BL_ALLOWLIST_MAX`=16 BSSIDs). A
  convenience to confine a test, **not** an authorization credential. Exists in
  the core only — **no wire message sets it yet** (DESIGN).

### Active-op request + check order

`CMD_REQ` for an active capability: `[u16 cap_id][u8 channel][u8 bssid[6]]`;
`channel` 1–14. Checked in order: e-stop (`estopped`) → active ops enabled
(`not_authorized`) → args (`bad_arg`) → frame budget (`rate_limited`) → operator
scope (`target_not_allowed`).

> **Current shipping behaviour (not a design limit to soften):** the StickS3
> refuses **every** active op today. An active op needs Wi-Fi, so it must run as
> a job, and active jobs are refused (`capability_disabled`) until ADR 0006's
> active-job rules exist and are tested. A bridge holding the demo key refuses
> every active op regardless. BruceLink builds **no RF frames** — it forwards to
> Bruce's primitives (`bruce_brucelink_deauth`), and the gate is enforced before
> Bruce is reached.

SW-VERIFIED: gate logic + budget + check-order in `tests/c/test_bridge.cpp`.

---

## 10. Wire error codes (`bl_proto.h` `bl_wire_err_t`; ERROR payload `[u16]`)

0 none · 1 bad_frame · 2 unsupported_version · 3 not_authenticated · 4
unknown_type · 5 bad_arg · 6 unknown_capability · 7 capability_disabled · 8
not_authorized (active ops not enabled, or the demo key) · 9 target_not_allowed ·
10 rate_limited · 11 busy · 12 no_resource · 13 not_found · 14 estopped · 15
internal · 16 use_job (this capability runs as a job — `CMD_REQ` on a JOB
capability returns this). Text via `bl_wire_err_str`.

Internal result codes (`bl_result_t`, `bl_types.h`) are **never sent on the
wire**: `BL_OK`, `BL_E_ARG`, `…E_MAC`, `…E_REPLAY`, `…E_STATE`, `…E_AUTH`,
`…E_CRYPTO`, etc. — library return values only.

---

## 11. Fragmentation (BLE) and COBS (serial)

### BLE fragmentation (`bl_reasm.{h,c}`)

A frame (≤ 540 B) is split into chunks sized to the negotiated ATT MTU − 3. Each
chunk: `[u8 flags: 0x01 FIRST, 0x02 LAST][u8 index][data…]` (`BL_FRAG_HDR`=2).
BLE preserves message boundaries per write/notify, so this layer only
splits/reassembles — no byte-stream resync. `bl_reasm_push` requires FIRST,
contiguous wrapping indices, and bounds the total to `BL_MAX_MSG`; a bad index /
overflow / missing FIRST resets and returns `BL_E_SEQ`/`BL_E_OVERFLOW`/
`BL_E_FORMAT`. Emit with `bl_frag_chunk` (iterate until it returns 1).

Chunk sizing (`bl_uuid.h`): `BL_BLE_CHUNK_MAX`=244 (the Flipper's stack caps
characteristic updates below 250 and inbound data at 245/event),
`BL_BLE_CHUNK_DEFAULT`=20 (ATT MTU 23 − 3), `bl_ble_chunk_for_mtu(att_mtu)` =
`min(att_mtu − 3, 244)`. The Flipper RX assembler (`bl_rx_asm_*` in
`bl_gatt_evt.h`) reassembles a single GATT write delivered as multiple
attribute-modified events.

### GATT profile (`bl_uuid.h`, ADR 0001)

One custom 128-bit service, one definition for both devices (string form for
NimBLE, little-endian byte form for the STM32WB stack; `tests/c/test_client.c`
checks they agree):

```
service  a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00   (BL_UUID_SERVICE_STR)
RX char  a1b20002-…   central -> peripheral   write / write-no-response
TX char  a1b20003-…   peripheral -> central   notify
```

No BLE-level pairing/bonding (`GapPairingNone`, open attribute permissions); all
integrity comes from the BruceLink session. "Link up" = the central subscribes
to TX notifications.

### Serial COBS (`bl_cobs.{h,c}`)

For a UART byte stream: COBS-encode each frame, append a `0x00` delimiter; a
reader resyncs on the delimiter (`bl_cobs_stream_push`). **Not used on BLE**
(GATT writes are message-framed). No serial transport ships today (DESIGN).

---

## 12. Session lifetime, deadlines, cooldown

- A **new authenticated session** resets the gate: e-stop cleared, active ops
  disabled until re-enabled. The rate window is **not** reset (device transmit
  history; only elapsed time expires it). `BruceLinkBridge::onConnect` resets
  the gate only once the session authenticates, never on a bare link.
- **Link loss** = automatic e-stop + capture stop + session-secret wipe
  (`onDisconnect`), fail-safe.
- **Handshake deadline.** The StickS3 drops a link not authenticated within
  `kAuthTimeoutMs`=10 000 ms of connect; a forged proof drops it immediately.
  A dropped/forged peer address is put on cooldown (`NimBleCentralTransport::
  kPeerCooldownMs`=30 000 ms, `PeerCooldown`), so the next scan moves on. A
  genuinely-pairing peer is **not** penalized (retry at once). The Flipper must
  send HELLO the instant the StickS3 enables TX notifications.

HW-OWNER: a paired reconnect after reboot and link/handshake behaviour
(2026-11-23, `../TEST_RESULTS.md`).

---

## 13. GAPS / cautions

- **ADR 0002 transcript line is stale** (§4): it omits the version range. The
  code includes it; trust `bl_session.{h,c}`.
- **ADR 0002 frame table calls byte 5 "reserved"** (§2): it is the request tag
  in the code.
- **Bridge dispatch bodies not line-read.** The per-message handlers in
  `esp32/lib/BruceLink/BruceLinkBridge.cpp` (`handleCommand`, `handleActiveOp`,
  `handleJobStart`, `handleFile*`) were confirmed by signature and the
  registry/limit tables, not read line-by-line; exact byte-level error ordering
  beyond §9 should be re-read there if load-bearing.
- **Operator target scope** exists only in `bl_safety` core; no wire message
  sets it (DESIGN).
- **Confidentiality (M4.6):** frames are authenticated, not encrypted — repeated
  here because it governs how capture downloads and status must be described.
- All hardware behaviour is **HW-OWNER**, not independently verified in this
  session.
