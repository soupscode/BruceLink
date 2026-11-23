# ESP32-S3 / Bruce bridge

The StickS3 side of BruceLink. It is a BLE **central** that connects to the
Flipper (BLE peripheral), runs the authenticated session, and forwards requests
to Bruce's own functions.

## Layout

- `lib/BruceLink/` — reusable, portable:
  - `BruceLinkBridge.{h,cpp}` — transport/firmware-independent bridge core
    (session + safety gate + capture manager + dispatch). Depends only on the
    shared protocol core (pulled in by include path / generated wrappers) and an abstract
    `BruceBackend`. Unit-testable off-device.
  - `NimBleCentralTransport.{h,cpp}` — NimBLE-Arduino 2.5.1 central: scan,
    connect, discover RX/TX chars, bridge notify↔write.
- `src/modules/brucelink/` — the Bruce glue (compiles **only** inside a Bruce
  `1.17` checkout): `BruceBackendImpl` (calls Bruce's device info / Wi-Fi scan /
  sniffer / transmit), `bruce_brucelink_hooks.h` (the thin headless additions),
  and `PATCH.md` (exactly what to change in Bruce).
- `test_compile/` — a standalone PlatformIO project that compiles the bridge +
  NimBLE + shared core with a stub backend, to prove it builds for the ESP32-S3.
  **Not** the Bruce firmware.

## Build the compile-check

```sh
cd test_compile && pio run    # board esp32-s3-devkitc-1, NimBLE via GitHub tag 2.5.1
```

## Integrate into Bruce

Follow `src/modules/brucelink/PATCH.md`: drop `lib/BruceLink` into Bruce's `lib/`,
add `src/modules/brucelink/`, expose the headless sniffer hooks, add a menu
toggle, and provision the pairing key in `/brucelink.json` (never `bruce.conf`).
Board target: `m5stack-sticks3` (8 MB flash, 3 MB LittleFS, no built-in SD).

## Flashing & recovery

Back up full flash first: `../scripts/hil/backup_sticks3.sh`. Bruce ships a
merged `Bruce-m5stack-sticks3.bin`; see `../docs/HARDWARE_TESTING.md`.
