# BruceLink

Native Flipper Zero (Momentum) remote frontend for a backpack-mounted
M5StickS3 (ESP32-S3) running Bruce firmware, over an authenticated BLE link.
BruceLink **reuses Bruce's existing functions** through a remote interface — it
does not reimplement them.

> **Status: foundation complete and host-verified; nothing verified on physical
> hardware yet.** The BLE link is designed and the protocol is built and tested,
> but the Flipper BLE transport is still a stub. See
> [`docs/STATUS.md`](docs/STATUS.md) and [`LOCAL_HANDOFF.md`](LOCAL_HANDOFF.md).

## What works today (verified in CI)

- A portable C99 **protocol core** shared byte-for-byte by both devices:
  authenticated session (pre-shared key + ephemeral X25519, forward secrecy,
  replay protection), framing, fragmentation, a capability registry, a capture
  session manager, a pcap writer, and a remote-operation safety gate.
- **1434** host assertions under AddressSanitizer + UBSan, cppcheck clean, and a
  C↔Python wire-interop check.
- The **Flipper app** compiles for Momentum `mntm-012` (and runs end-to-end
  against an in-process mock over a loopback transport).
- The **ESP32-S3 bridge** + NimBLE 2.5.1 central compiles and links for the
  target.

## Layout

| Path | What |
|---|---|
| `shared/brucelink_core/` | portable protocol core (C99) |
| `flipper/brucelink/` | native Flipper FAP (UI + transports) |
| `esp32/lib/BruceLink/` | bridge core + NimBLE central transport |
| `esp32/src/modules/brucelink/` | Bruce glue + `PATCH.md` (builds in the Bruce tree) |
| `esp32/test_compile/` | standalone ESP32-S3 compile check |
| `tests/` | host C tests + Python reference/interop |
| `scripts/hil/` | hardware-in-the-loop scripts |
| `docs/` | ADRs, architecture, protocol, status, capability matrix, HW testing |

## Build & test

```sh
make -C tests/c test cppcheck            # core tests (ASan+UBSan) + static analysis
python3 -m unittest discover -s tests    # Python reference + C interop
(cd flipper/brucelink && ufbt)           # Flipper FAP  (needs the mntm-012 SDK)
(cd esp32/test_compile && pio run)       # ESP32-S3 bridge compile check
scripts/hil/build_all.sh                 # all of the above
```

## Pinned upstream

Momentum [`mntm-012`](https://github.com/Next-Flip/Momentum-Firmware) (API 87.1)
· Bruce [`1.17`](https://github.com/BruceDevices/firmware) (env `m5stack-sticks3`)
· NimBLE-Arduino 2.5.1. BruceLink is independent of these projects.

## Safety

Active, RF-transmitting functions (e.g. deauthentication testing) are
**disabled by default**, gated behind an authenticated connection plus an
explicit operator opt-in, a target allowlist, a rate limit, and an emergency
stop that also fires on link loss. They are intended only for owned or
explicitly authorized, isolated lab environments. BruceLink adds no transmit
code of its own; it forwards to Bruce's existing primitives. See
[`docs/decisions/0003-capture-and-safety.md`](docs/decisions/0003-capture-and-safety.md).

## License

MIT — see [`LICENSE`](LICENSE).
