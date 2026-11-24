# Test builds (branch `test/capture`, not for `main`)

Built from `448b798` (`feat/radio-handoff`, PR #8): handshake capture as a job
and capture files on the Flipper. **Not yet run on hardware.** Both devices
must run these builds together.

| File | Device | How |
|---|---|---|
| `brucelink.fap` | Flipper (Momentum `mntm-012`, API 87.1), app v0.7 | copy to `SD:/apps/Bluetooth/brucelink.fap` |
| `Bruce-BruceLink-m5stack-sticks3-FULL.bin` | StickS3 (Bruce 1.17 + BruceLink) | `esptool --chip esp32s3 write-flash 0x0 <file>`; erases the pairing: pair again |

`SHA256SUMS` lists both. Captures cross BLE unencrypted until M4.6: test with
your own network only. Steps: `docs/HARDWARE_TESTING.md` 7 and 8.
