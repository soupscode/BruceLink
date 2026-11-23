# ADR 0001 — BLE transport: Flipper peripheral, StickS3 central, FAP-owned custom GATT profile

Status: Accepted — implemented on both devices (M4.2), hardware-unverified. Amended 2026-11-23: see "Amendment — M4.2 implementation".
Date: 2026-11-23
Pinned sources: Momentum `mntm-012` (`e1784e7`), copro submodule `stm32wb_copro` @ `133182d` (Cube 1.20.0, `ble_light` stack); Bruce `1.17` (`2b6ba4d`), NimBLE-Arduino 2.5.1.

## Context

BruceLink needs a wireless link between a Flipper Zero (Momentum) held by the
operator and a backpack-mounted M5StickS3 (ESP32-S3) running Bruce. The project
brief prefers BLE and minimal upstream firmware changes. We verified the actual
upstream capabilities against source rather than assuming them.

## Findings (verified in source)

- **Flipper is peripheral-only.** Momentum ships the *light* copro stack
  (`fbt_options.py`: `COPRO_STACK_TYPE="ble_light"`) and initialises GAP with
  `GAP_PERIPHERAL_ROLE` (`targets/f7/ble_glue/gap.c`). No scan/central API is
  exported in `targets/f7/api_symbols.csv` (API 87.1). → The **ESP32-S3 must be
  the BLE central**; nothing prevents a non-phone central connecting.
- **A FAP can own a custom 128-bit GATT profile.** `ble_gatt_service_add`,
  `ble_gatt_characteristic_init/_update`, `bt_profile_start`,
  `bt_profile_restore_default`, `bt_keys_storage_set_storage_path`, and
  `ble_event_dispatcher_register_svc_handler` are all exported. The in-tree
  HID/BadKB "ext profile" (`applications/.../ble_hid_ext_profile.c`) is the
  pattern to copy, including a custom MAC, name, and pairing mode.
- **Reusing the stock serial profile is wrong for us.** On connect the bt
  service opens an RPC session and overwrites the serial callback
  (`bt.c`), its characteristics require `AUTHEN` permissions, and TX is
  indication-only. A FAP-owned profile avoids all three.
- **MTU / sizing.** `CFG_BLE_MAX_ATT_MTU` = 414. A FAP characteristic update
  goes through `aci_gatt_update_char_value` whose length is a `uint8_t`
  (≤ 249); inbound `Attr_Data` is ≤ 245 per event. → **Keep chunks ≤ 244
  bytes** and fragment above that (we do, in `bl_reasm`).
- **Pairing.** `GapPairingNone` ("just works") with `ENCRY` (not `AUTHEN`)
  characteristic permissions suits a headless central. Caveat: the app's
  `Connected` status only fires on `ACI_GAP_PAIRING_COMPLETE`, so the ESP32
  should initiate Just-Works pairing (verify on hardware). *Superseded for
  bring-up: no BLE pairing at all — see the amendment.*
- **Key storage isolation.** `bt_keys_storage_set_storage_path(APP_DATA_PATH(
  ".brucelink.keys"))` keeps BruceLink's bonds separate from the phone bond.
  Starting a custom profile disconnects the phone/RPC; on exit the app must
  `bt_profile_restore_default()` and unregister its GATT handler (follow
  `applications/system/hid_app/hid.c` exactly).

## Decision

1. **Roles:** Flipper = BLE **peripheral** advertising a custom 128-bit
   BruceLink service; StickS3 = BLE **central** (NimBLE) that scans, connects,
   and subscribes.
2. **Profile:** one service, two characteristics —
   - RX `…9d00/0002`: `WRITE | WRITE_NO_RSP`, `GATT_NOTIFY_ATTRIBUTE_WRITE`
     (central → peripheral).
   - TX `…9d00/0003`: `NOTIFY` (peripheral → central).
   Both variable length, ≤ 244 bytes. UUIDs and chunk limits live in one
   header shared by both devices (`bl_uuid.h`). Permissions: see the amendment.
3. **Security role split is independent of the BLE role:** the *session*
   handshake (ADR 0002) is initiated by the Flipper (session "central") once
   the BLE link is up; the ESP32 is the session "peripheral"/responder. BLE
   pairing (Just Works) protects the link; the session layer provides the
   actual mutual authentication and replay protection, so we do **not** depend
   on BLE `AUTHEN`/MITM.
4. **Fragmentation:** messages > one chunk are split with `bl_reasm`
   (2-byte fragment header), one chunk per GATT write/notify.
5. **Transfer path for bulk PCAP (ADR 0003):** not BLE. See that ADR.

6. **Link supervision (added after PR #1 review).** The service UUID is public
   and the StickS3 connects to whatever advertises it, so a BLE connection is
   never trusted on its own:
   - the bridge must reach an authenticated session within **10 s**
     (`kAuthTimeoutMs`); a forged proof, a version mismatch or an unprovisioned
     bridge ends the attempt immediately;
   - on either, the transport disconnects and puts that peer's address on a
     **30 s cooldown** (`PeerCooldown`), so the scan moves on instead of
     reconnecting to the same impostor;
   - **requirement on the Flipper BLE transport:** start the session handshake
     (send `HELLO`) as soon as the StickS3 enables TX notifications, not on a
     later user action. Otherwise the deadline drops the real Flipper.
   - Residual risk: an attacker that rotates its address on every attempt can
     still *delay* (never hijack) the connection, like any BLE jammer.
     Mitigation option for later: pin the paired Flipper's address after the
     first successful session (the FAP profile sets a fixed MAC).
7. **Threading:** NimBLE callbacks only record events; the application task's
   `loop()` is the sole owner of the bridge, and the bridge is brought up
   *before* notifications are subscribed so an immediate `HELLO` is never lost.

## Amendment — M4.2 implementation (2026-11-23)

What was built differs from the original decision in one place, and adds
details that only became clear while writing the Flipper profile.

- **No BLE-level pairing or encryption (bring-up).** The Flipper profile uses
  `GapPairingNone`, no bonding and `ATTR_PERMISSION_NONE`; the NimBLE central
  never initiates pairing. The two ends agree, and there is no pairing
  procedure to fail between a headless central and the Flipper's stack.
  - Unchanged: every frame is authenticated and replay-protected by the
    session (ADR 0002). An attacker without the pairing key cannot issue
    commands, forge replies or replay old ones.
  - **Lost versus the original decision: confidentiality.** Payloads (device
    info, scan results, status) are readable by a nearby BLE sniffer. Just
    Works pairing would only have protected against passive listeners, not an
    active man-in-the-middle, so it was never the security boundary.
  - Follow-up **M4.6**: add confidentiality at the session layer (AEAD with the
    per-direction keys the handshake already derives). That covers every
    transport, including the planned UART one, which BLE pairing would not.
- **Link-up is the TX subscription, not the firmware's `Connected` status**
  (which only fires after pairing). The transport reports a new link each time
  the central enables notifications, and the app sends `HELLO` in the same
  tick (decision 6).
- **Handshake recovery is a link restart, not a retransmit (M3.5).** If the
  handshake has not completed 5 s after `HELLO`, the Flipper drops the BLE
  link and advertises again. 5 s is inside the StickS3's 10 s deadline,
  and a peer that disconnects itself is not put on the cooldown list, so a
  lost frame costs one reconnect instead of 10 s + 30 s. Requests time out
  after 10 s (a Wi-Fi scan is answered only once it has run) and leave the
  session up. Both values are estimates: the Flipper's X25519 time and
  Bruce's scan time have not been measured on hardware.
- **Dropping a link from the Flipper stops advertising.** `bt_disconnect()`
  ends the connection through `gap_stop_advertising()`, which clears the
  firmware's restart-advertising flag. The transport restarts advertising
  after every drop and re-checks on each poll.
- **Own address and key file.** The profile advertises on the Flipper's BLE
  address with byte 2 + 2 (the HID profile uses + 1), name `BL` (a 128-bit
  UUID leaves at most five characters in a 31-byte advertisement, and a
  rejected advertisement fails silently, so two are used for margin), and points the key
  store at `.brucelink.keys` so the phone bond is untouched.
- **Events are parsed by a portable, host-tested parser** (`bl_gatt_evt`):
  attribute-modified → RX data (value handle = characteristic handle + 1) or
  subscription (CCCD = handle + 2); MTU response; connection complete;
  disconnect.
- **A central that connects but does not subscribe is dropped after 5 s.**
  The radio does not advertise while connected and the GATT is open, so any
  BLE device could otherwise hold the Flipper's only link.
- **Chunk size is one shared rule** (`bl_ble_chunk_for_mtu`): MTU − 3, capped
  at 244, 20 until an MTU is known. The Flipper's stack accepts MTUs up to
  414 but its characteristics carry 244 bytes.
- **Starting the transport advertises even if Bluetooth is off in the
  Flipper's settings** (same as the stock HID app); the setting applies again
  when the default profile is restored on exit.
- **The firmware can take the radio back** (it reloads the default profile
  when the SD card is remounted). The transport detects that its services
  are gone, reports the link down and stops touching the radio until the
  user presses Connect again.
- **Independent review (2026-11-23)** checked the profile against the pinned
  Momentum, ST and NimBLE sources: event layouts and handle offsets,
  lifecycle `furi_check`s, threading, TX callback use and interop were found
  consistent; its robustness findings are the items above. Open questions
  that only hardware can answer are listed in `docs/HARDWARE_TESTING.md` §5.

## Consequences

- Minimal Momentum change: **none to the core BT stack**; BruceLink is a FAP
  with its own profile. Minimal Bruce change: a NimBLE central bridge added as
  a library + a small glue module (ADR 0003).
- The phone BLE/RPC link is unavailable while the BruceLink profile is active
  (single connection handle). Documented for the operator.
- **Must be hardware-verified:** an unpaired NimBLE central subscribing and
  writing; advertising a 128-bit UUID plus a short name within 31 bytes;
  re-advertising after a Flipper-initiated drop; negotiated MTU
  ≥ 247 and notify throughput; write fragmentation above 245 bytes;
  reconnection after `restore_default` and after the Flipper's core-2 restart.
- The inverted return value of `ble_gatt_characteristic_update` (true on
  failure) and the FAP-defined ST event structs are implementation hazards
  noted for the Flipper-side profile work.

## Alternatives considered

- **Reuse stock serial/RPC profile** — rejected (RPC hijack, AUTHEN, indicate-only).
- **Bluetooth Classic SPP** — impossible; ESP32-S3 has no BT Classic.
- **Wi-Fi link instead of BLE** — heavier and conflicts with Bruce's Wi-Fi
  radio use; reconsidered only as an optional bulk-transfer channel (ADR 0003).
