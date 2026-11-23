# BruceLink experimental wire protocol v0

This is **not** an existing Bruce protocol.

Frame layout:
- ASCII `BL` magic (2 bytes)
- Version 1 (1 byte)
- Message type (1 byte)
- Payload length, unsigned little endian (2 bytes, maximum 512)
- Payload (0..512 bytes)
- CRC-16/CCITT-FALSE, little endian (2 bytes), over all preceding bytes

Message types: PING=0x01, PONG=0x02, STATUS_REQUEST=0x03, STATUS_RESPONSE=0x04, WIFI_SCAN_REQUEST=0x10, WIFI_SCAN_RESULT=0x11, ERROR=0x7f.

Before BLE implementation, add transaction IDs, fragmentation, authentication, replay prevention, timeouts and cancellation. Untrusted inputs must never directly invoke unrestricted Bruce actions.
