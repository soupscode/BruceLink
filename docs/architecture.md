# Architecture

Flipper native FAP -> authenticated BLE transport -> StickS3 bridge -> verified Bruce APIs.

Flipper owns UI and logs; StickS3 owns radio operations and independent power. The shared protocol should be tested over serial before adding BLE.

**BLE roles are not yet validated.** Investigate Momentum's Flipper peripheral interfaces and ESP32-S3 central support; do not assume generic BLE serial compatibility. Bluetooth Classic SPP is not supported on ESP32-S3.

Security: local pairing and authorization, allowlisted commands, bounded payloads, cancellation, timeouts, and rate limits. Restrict active wireless experiments to owned or explicitly authorized lab networks.
