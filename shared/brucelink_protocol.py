"""BruceLink experimental transport-independent frame codec."""
MAGIC = b"BL"
VERSION = 1
MAX_PAYLOAD = 512

def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)) & 0xFFFF
    return crc

def encode(kind: int, payload: bytes = b"") -> bytes:
    if not 0 <= kind <= 255 or len(payload) > MAX_PAYLOAD:
        raise ValueError("invalid kind or payload length")
    frame = MAGIC + bytes((VERSION, kind)) + len(payload).to_bytes(2, "little") + payload
    return frame + crc16(frame).to_bytes(2, "little")

def decode(packet: bytes) -> tuple[int, bytes]:
    if len(packet) < 8 or packet[:2] != MAGIC or packet[2] != VERSION:
        raise ValueError("invalid header")
    length = int.from_bytes(packet[4:6], "little")
    if length > MAX_PAYLOAD or len(packet) != length + 8:
        raise ValueError("invalid length")
    if crc16(packet[:-2]) != int.from_bytes(packet[-2:], "little"):
        raise ValueError("CRC mismatch")
    return packet[3], packet[6:-2]
