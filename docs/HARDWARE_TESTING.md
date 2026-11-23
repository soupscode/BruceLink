# Hardware-in-the-loop plan

Use Claude Code on a local workstation with both devices connected by USB.

1. Record exact Momentum version, Bruce commit, StickS3 board revision, and serial port paths.
2. Run `python3 -m unittest discover -s tests -v`.
3. Compile the Flipper FAP against a pinned Momentum-compatible SDK.
4. Back up current StickS3 settings/firmware before flashing a verified board target.
5. Validate serial PING/PONG and record raw bytes.
6. Validate BLE pairing, authentication, reconnection, fragmentation and malformed packet handling.
7. Test status and passive Wi-Fi scan on authorized equipment.
8. Store build logs, serial logs, and commit hashes with each test result.

qFlipper is a GUI companion, not a general automated test API. Use documented CLI/RPC and USB serial capabilities. No automatic flashing is provided in this scaffold.
