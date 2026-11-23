"""BruceLink protocol v1 — Python reference implementation.

A second, independent implementation of the BruceLink wire format used
to verify byte-for-byte interoperability with the C core
(shared/brucelink_core). It covers the frame codec + MAC, COBS framing,
the PCAP headers and CRC32. The authenticated session handshake
(X25519/HKDF) is exercised by the C known-answer tests against RFC 7748
and RFC 5869; this module focuses on the wire encodings where
cross-implementation bugs (endianness, offsets, truncation) live.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import hashlib
import hmac
import struct
from dataclasses import dataclass

MAGIC = b"BL"
WIRE_VERSION = 1
HDR_LEN = 12
MAC_TAG_LEN = 16
MAX_PAYLOAD = 512

# Header flags
FLAG_NONE = 0x00
FLAG_RESPONSE = 0x01
FLAG_ERROR = 0x02
FLAG_EVENT = 0x04


def mac_tag(key: bytes, hdr_and_payload: bytes) -> bytes:
    """Truncated HMAC-SHA256 matching bl_frame.c."""
    return hmac.new(key, hdr_and_payload, hashlib.sha256).digest()[:MAC_TAG_LEN]


@dataclass
class Frame:
    version: int
    type: int
    flags: int
    seq: int
    payload: bytes


def encode(frame: Frame, key: bytes | None = None) -> bytes:
    """Encode a frame. With key, append a MAC; without, append zero tag."""
    if len(frame.payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    hdr = (
        MAGIC
        + bytes((frame.version, frame.type, frame.flags, 0))
        + struct.pack("<I", frame.seq)
        + struct.pack("<H", len(frame.payload))
    )
    body = hdr + frame.payload
    tag = mac_tag(key, body) if key is not None else bytes(MAC_TAG_LEN)
    return body + tag


def decode(packet: bytes, key: bytes | None = None) -> Frame:
    """Decode and validate a frame; verify MAC when key is given."""
    if len(packet) < HDR_LEN + MAC_TAG_LEN:
        raise ValueError("short")
    if packet[0:2] != MAGIC:
        raise ValueError("bad magic")
    plen = struct.unpack_from("<H", packet, 10)[0]
    if plen > MAX_PAYLOAD:
        raise ValueError("payload too large")
    if len(packet) != HDR_LEN + plen + MAC_TAG_LEN:
        raise ValueError("bad length")
    if key is not None:
        want = mac_tag(key, packet[: HDR_LEN + plen])
        got = packet[HDR_LEN + plen :]
        if not hmac.compare_digest(want, got):
            raise ValueError("MAC mismatch")
    return Frame(
        version=packet[2],
        type=packet[3],
        flags=packet[4],
        seq=struct.unpack_from("<I", packet, 6)[0],
        payload=packet[HDR_LEN : HDR_LEN + plen],
    )


# --- COBS (matches bl_cobs.c) ---------------------------------------------


def cobs_encode(data: bytes) -> bytes:
    """COBS-encode and append a 0x00 delimiter."""
    out = bytearray()
    code_pos = 0
    out.append(0)  # placeholder for first code
    code = 1
    for b in data:
        if b != 0:
            out.append(b)
            code += 1
            if code != 0xFF:
                continue
        out[code_pos] = code
        code_pos = len(out)
        out.append(0)
        code = 1
    out[code_pos] = code
    out.append(0)  # delimiter
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Decode a COBS frame (without the trailing delimiter)."""
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0:
            raise ValueError("zero code")
        i += 1
        if i + (code - 1) > n:
            raise ValueError("overrun")
        out += data[i : i + code - 1]
        i += code - 1
        if code != 0xFF and i < n:
            out.append(0)
    return bytes(out)


# --- PCAP + CRC32 (match bl_pcap.c) ---------------------------------------

PCAP_MAGIC = 0xA1B2C3D4
PCAP_LINKTYPE_IEEE802_11 = 105
PCAP_DEFAULT_SNAPLEN = 2500


def pcap_global_header(snaplen: int = PCAP_DEFAULT_SNAPLEN,
                       linktype: int = PCAP_LINKTYPE_IEEE802_11) -> bytes:
    return struct.pack("<IHHiIII", PCAP_MAGIC, 2, 4, 0, 0, snaplen, linktype)


def pcap_record_header(ts_sec: int, ts_usec: int, incl: int, orig: int) -> bytes:
    return struct.pack("<IIII", ts_sec, ts_usec, incl, orig)


def crc32(data: bytes) -> int:
    """CRC-32/ISO-HDLC, matching bl_crc32()."""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


# --- Pairing derivations (ADR 0005) ---------------------------------------
# Written from the ADR, not from bl_pair.c, so the interop test checks the
# C code against the specification. X25519 itself is covered by the C
# RFC 7748 vectors; the test feeds in the shared secret the C side found.

PAIR_VERSION = 1
PAIR_CODE_MOD = 1_000_000
MSG_PAIR_COMMIT = 0x08
MSG_PAIR_KEY = 0x09
MSG_PAIR_REVEAL = 0x0A
MSG_PAIR_CONFIRM = 0x0B


def _h(*parts: bytes) -> bytes:
    return hashlib.sha256(b"".join(parts)).digest()


def pair_commitment(pk_i: bytes, n_i: bytes) -> bytes:
    return _h(b"BLpair1 commit", bytes((PAIR_VERSION,)), pk_i, n_i)


def pair_transcript(name_i: bytes, pk_i: bytes, n_i: bytes,
                    name_r: bytes, pk_r: bytes, n_r: bytes) -> bytes:
    return _h(b"BLpair1 transcript", bytes((PAIR_VERSION, len(name_i))), name_i, pk_i, n_i,
              bytes((len(name_r),)), name_r, pk_r, n_r)


def pair_code(transcript: bytes) -> int:
    return int.from_bytes(_h(b"BLpair1 code", transcript)[:4], "little") % PAIR_CODE_MOD


def _hkdf(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, i = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes((i,)), hashlib.sha256).digest()
        out += block
        i += 1
    return out[:length]


def pair_psk(shared: bytes, n_i: bytes, n_r: bytes, transcript: bytes) -> bytes:
    return _hkdf(n_i + n_r, shared, b"BLpair1 psk" + transcript, 32)


def pair_confirm(psk: bytes, transcript: bytes, initiator: bool) -> bytes:
    label = b"BLpair1 confirm I" if initiator else b"BLpair1 confirm R"
    return hmac.new(psk, label + transcript, hashlib.sha256).digest()


def pair_key_id(psk: bytes) -> bytes:
    return _h(b"BLpair1 key id", psk)[:2]
