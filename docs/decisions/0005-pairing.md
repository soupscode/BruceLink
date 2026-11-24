# ADR 0005 — Pairing: key agreement with a 6-digit comparison code

Status: Accepted (2026-11-23) — implemented on both devices; **ran on the
owner's devices** (2026-11-23: pairing completed, and the paired key connected
after a StickS3 reboot; key-id display, refusal and Forget not yet checked). Replaces the public demo key on real devices (task M5.3).

## Context

Every session is authenticated with a 32-byte pre-shared key (ADR 0002). Until
now both builds compiled in the same published demo key, so the authentication
proved nothing, and the bridge refused all active operations because of it
(ADR 0003 amendment). The devices need a way to agree on a private key.

Constraints: the StickS3 has three buttons and a small screen, the Flipper a
d-pad; typing a 64-character hex key is not an option. No server, no phone app.
The BLE link itself is unpaired (ADR 0001 amendment), so the key exchange runs
over a channel an attacker can read and modify.

## Decision

A key agreement in the shared core (`bl_pair`), run only when the user puts
both devices into pairing mode, protected against a man in the middle by the
user comparing a 6-digit code shown on both screens — the same idea as
Bluetooth's "numeric comparison".

```
I = initiator (Flipper), R = responder (StickS3); H = SHA-256

I→R PAIR_COMMIT  ver | commit = H("BLpair1 commit" | ver | pk_I | n_I) | len | name_I
R→I PAIR_KEY     ver | pk_R | n_R | len | name_R
I→R PAIR_REVEAL  pk_I | n_I                    R checks commit; I checks pk_R is usable
     both        T    = H("BLpair1 transcript" | ver | lenI | name_I | pk_I | n_I
                                                     | lenR | name_R | pk_R | n_R)
                 code = LE32(H("BLpair1 code" | T)[0..3]) mod 1 000 000
     user        compares the two codes, answers on each device
I↔R PAIR_CONFIRM HMAC(psk, "BLpair1 confirm I" | T)  /  "... confirm R"   (after accept)
I↔R PAIR_ABORT   reason (1 rejected, 2 cancelled, 3 timeout, 4 key/commit check failed)

psk    = HKDF-SHA256(salt = n_I | n_R, ikm = X25519(sk, pk_peer), info = "BLpair1 psk" | T)
key id = H("BLpair1 key id" | psk)[0..1]   (display only)
```

- **Why the commitment.** I fixes its public key before it sees R's, and R
  reveals its key before it sees I's. A man in the middle running the protocol
  separately with each side must fix its own key on at least one side before it
  can know the code that side will show, so it gets the two codes to agree with
  probability 10⁻⁶ per attempt — and every attempt needs both users to accept a
  code. Without the commitment it could try keys until the codes match.
- **Why confirm tags.** They make each side prove it holds the same key, and
  they are only sent after the local user accepted. A side that accepted keeps
  nothing unless the peer's tag checks out; if the codes differed and a user
  accepted by mistake, the tags fail and nothing is stored.
- **Names** are shown on the other screen ("StickS3: M5StickS3"), are covered by
  the code, are at most 16 printable ASCII characters, and are refused
  otherwise.
- **Frames** carry a zero MAC field, like the session handshake: there is no
  key yet. Message types 0x08–0x0C. Every frame is under 128 bytes.
- **One attempt per BLE link**, 90 s limit. A failed attempt wipes its secrets.
  Frames out of order or malformed are ignored; a wrong commitment, a wrong
  confirm tag or an unusable key fail the attempt.
- **Modes are separate.** A StickS3 in pairing mode never authenticates a
  session (even with the right key) and one in normal mode never answers
  pairing. A non-pairing Flipper reaching a StickS3 in pairing mode is dropped
  and put on the cooldown list; a Flipper whose attempt failed is not, so the
  user can retry at once.

### Storage

| Device | Where | Protection |
|---|---|---|
| Flipper | `apps_data/brucelink/pairing.key` (SD) | Encrypted with the Flipper's device-unique enclave key, like the stock U2F app. A copy of the file is useless on another Flipper; on load a key id is checked, so a foreign or damaged file is reported, not used. |
| StickS3 | NVS (`Preferences`, namespace `brucelink`) | Not in `bruce.conf` (printed to serial) and not on LittleFS (readable through Bruce's file browser and web UI), the same place Bruce keeps its U2F secrets. Flash encryption is off, so physical access with a USB cable can read it. |

Flashing the StickS3's merged image (offset 0x0) erases NVS and with it the
pairing. Flashing its parts separately (`bootloader.bin` at 0x0,
`partitions.bin` at 0x8000, `firmware.bin` at 0x10000) would write the same
bytes except NVS (0x9000) and so keep the pairing, but on the owner's StickS3 it
did not boot, twice (2026-11-23, cause unknown). Until one such flash boots,
updates mean the merged image and pairing again.

### On the devices

- Flipper: **Pair** (first in the menu until paired), then **Pair again** /
  **Forget pairing**. Connect requires a pairing. The app is BLE-only and
  contains no demo key (its loopback mock, the last user of the demo key on the
  Flipper, was removed in v0.3).
- StickS3: **BruceLink** on Bruce's main menu (first item) → **Pair with
  Flipper** / **Forget pairing** / **Turn on**. Turning on requires a pairing;
  the demo key is not used on the device. After a successful pairing BruceLink
  switches to normal mode with the new key **without restarting BLE**: a
  restart needs a fresh 48 kB free block (Bruce's `radioHasMemForBle`), and on
  hardware the first version's stop-and-start after pairing failed for lack of
  memory. Leaving a failed or cancelled pairing goes back to the old key the
  same way if BruceLink was on before. (First version: two entries under
  Config → Advanced; moved to the main menu on the owner's request.)

## Consequences

- Real devices no longer use a public key: the session authentication now
  means something, and the bridge's demo-key lock (ADR 0003) no longer blocks
  active operations once paired. The safety gate still applies in full.
- Still open: **link confidentiality** (M4.6). The pairing key authenticates
  frames; it does not encrypt them.
- A user who accepts without looking defeats the code comparison. Both screens
  default to "No" (the StickS3's prompt) or require choosing "Yes" explicitly
  (the Flipper's buttons).
- Anyone with physical access to the StickS3 and a USB cable can read the key;
  anyone with the Flipper's SD card cannot (it is bound to that Flipper).
- Host-tested: `tests/c/test_pair.c` (13 tests, including a man in the middle
  and tampering with each field; six deliberately broken variants of the code
  are each caught), `test_bridge.cpp` pairing tests (5), and an independent
  Python recomputation of every derived value (`tests/test_protocol.py`).
- Not host-testable and not yet run: the Flipper's key file (enclave), its
  pairing screen, the StickS3's NVS store and pairing screen.

## Amendment — review notes (2026-11-23)

- **Key file integrity (Flipper).** `pairing.key` is AES-CBC under the
  enclave key with no MAC; on load the 2-byte key id is checked. A damaged
  file passes that check about once in 65,536 tries and then fails the
  handshake, so it is not a way in, only a confusing error. An HMAC over the
  file would make it exact. The file is written to a temporary name and
  renamed, so a write cut short does not destroy the old key.
- **A save that fails after both sides confirmed** leaves the devices out of
  step: the side that saved has the new key, the other its old one (or none).
  Each side says so on its own screen ("saving the key failed"); the fix is to
  pair again.
- **Pair again while BruceLink is on** switches the running link to pairing
  mode instead of restarting BLE (a restart needs a fresh 48 kB block the heap
  may not have), and forgets earlier peer cooldowns. **Forget** refuses to
  report success unless the bridge task has stopped (so the key is gone from
  RAM too) and NVS was cleared; it also clears the last job's result.
