"""Host tests for the Python reference, plus a C<->Python interop check.

Run with: python3 -m unittest discover -s tests -v
The interop test builds and runs tools/bl_vectors.c and verifies the
Python reference produces byte-identical results for the same inputs.
"""
import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "shared"))

import brucelink_protocol as bl  # noqa: E402


class FrameTests(unittest.TestCase):
    def test_roundtrip_unauth(self):
        f = bl.Frame(bl.WIRE_VERSION, 0x05, 0, 42, b"hello")
        g = bl.decode(bl.encode(f))
        self.assertEqual((g.type, g.seq, g.payload), (0x05, 42, b"hello"))

    def test_roundtrip_mac(self):
        key = bytes(range(32))
        f = bl.Frame(bl.WIRE_VERSION, 0x13, bl.FLAG_RESPONSE, 7, bytes(range(32)))
        pkt = bl.encode(f, key)
        g = bl.decode(pkt, key)
        self.assertEqual(g.payload, bytes(range(32)))

    def test_mac_tamper(self):
        key = b"\x5a" * 32
        f = bl.Frame(bl.WIRE_VERSION, 0x13, 0, 1, b"abcdef")
        pkt = bytearray(bl.encode(f, key))
        pkt[bl.HDR_LEN + 2] ^= 1
        with self.assertRaises(ValueError):
            bl.decode(bytes(pkt), key)

    def test_length_and_magic(self):
        with self.assertRaises(ValueError):
            bl.decode(b"XX" + b"\x00" * 30)
        with self.assertRaises(ValueError):
            bl.encode(bl.Frame(1, 1, 0, 0, b"x" * 513))


class CobsTests(unittest.TestCase):
    def test_roundtrip(self):
        for data in (b"", b"\x00", b"\x11\x00\x22", bytes(range(256)), b"\x01" * 300):
            enc = bl.cobs_encode(data)
            self.assertEqual(enc[-1], 0)
            self.assertNotIn(0, enc[:-1])
            self.assertEqual(bl.cobs_decode(enc[:-1]), data)


class PcapTests(unittest.TestCase):
    def test_global_header(self):
        h = bl.pcap_global_header()
        self.assertEqual(h[:4], bytes([0xD4, 0xC3, 0xB2, 0xA1]))
        self.assertEqual(h[20], 105)

    def test_crc32_known(self):
        self.assertEqual(bl.crc32(b"123456789"), 0xCBF43926)


class InteropTests(unittest.TestCase):
    """Build the C vectors tool and verify Python matches it byte-for-byte."""

    @classmethod
    def setUpClass(cls):
        core = ROOT / "shared" / "brucelink_core" / "src"
        tool = ROOT / "tools" / "bl_vectors.c"
        out = ROOT / "tests" / "_bl_vectors"
        srcs = [str(tool)] + [str(p) for p in core.glob("*.c")]
        cc = os.environ.get("CC", "cc")
        try:
            subprocess.run(
                [cc, "-std=c99", "-O1", f"-I{core}", *srcs, "-o", str(out)],
                check=True, capture_output=True,
            )
        except (OSError, subprocess.CalledProcessError) as e:
            raise unittest.SkipTest(f"cannot build C vectors tool: {e}")
        cls.vectors = json.loads(subprocess.check_output([str(out)]))
        cls.out = out

    @classmethod
    def tearDownClass(cls):
        try:
            cls.out.unlink()
        except OSError:
            pass

    def test_frame_mac_interop(self):
        v = self.vectors
        key = bytes.fromhex(v["frame_key"])
        c_frame = bytes.fromhex(v["frame_mac"])
        # Python decodes and MAC-verifies the C-produced frame...
        f = bl.decode(c_frame, key)
        self.assertEqual(f.seq, 7)
        self.assertEqual(f.payload, bytes(range(32)))
        # ...and re-encodes to the identical bytes.
        py = bl.encode(bl.Frame(bl.WIRE_VERSION, f.type, f.flags, 7, f.payload), key)
        self.assertEqual(py, c_frame)

    def test_cobs_interop(self):
        v = self.vectors
        raw = bytes.fromhex(v["cobs_in"])
        self.assertEqual(bl.cobs_encode(raw), bytes.fromhex(v["cobs_out"]))

    def test_pcap_interop(self):
        v = self.vectors
        self.assertEqual(bl.pcap_global_header(), bytes.fromhex(v["pcap_global"]))
        self.assertEqual(
            bl.pcap_record_header(0x11223344, 0x000F4240, 100, 104),
            bytes.fromhex(v["pcap_record"]),
        )

    def test_crc32_interop(self):
        self.assertEqual(
            f"{bl.crc32(b'123456789'):08x}", self.vectors["crc32_123456789"]
        )

    def test_pairing_interop(self):
        """Every value the C pairing derives matches ADR 0005, recomputed here."""
        v = self.vectors
        self.assertTrue(v["pair_done"])
        f = {k: bl.decode(bytes.fromhex(v[k])) for k in
             ("pair_commit", "pair_key", "pair_reveal", "pair_confirm_i", "pair_confirm_r")}
        self.assertEqual(f["pair_commit"].type, bl.MSG_PAIR_COMMIT)
        self.assertEqual(f["pair_key"].type, bl.MSG_PAIR_KEY)
        self.assertEqual(f["pair_reveal"].type, bl.MSG_PAIR_REVEAL)
        # pairing frames carry a zero MAC field
        for k in f:
            self.assertEqual(bytes.fromhex(v[k])[-bl.MAC_TAG_LEN:], bytes(bl.MAC_TAG_LEN))

        c = f["pair_commit"].payload
        self.assertEqual(c[0], bl.PAIR_VERSION)
        commit, name_i = c[1:33], c[34:34 + c[33]]
        k = f["pair_key"].payload
        self.assertEqual(k[0], bl.PAIR_VERSION)
        pk_r, n_r, name_r = k[1:33], k[33:49], k[50:50 + k[49]]
        r = f["pair_reveal"].payload
        pk_i, n_i = r[:32], r[32:48]
        self.assertEqual(name_i, b"Flipper Aster")
        self.assertEqual(name_r, b"StickS3")

        self.assertEqual(bl.pair_commitment(pk_i, n_i), commit)
        t = bl.pair_transcript(name_i, pk_i, n_i, name_r, pk_r, n_r)
        self.assertEqual(bl.pair_code(t), v["pair_code"])
        psk = bl.pair_psk(bytes.fromhex(v["pair_shared"]), n_i, n_r, t)
        self.assertEqual(psk.hex(), v["pair_psk"])
        self.assertEqual(bl.pair_confirm(psk, t, True), f["pair_confirm_i"].payload)
        self.assertEqual(bl.pair_confirm(psk, t, False), f["pair_confirm_r"].payload)
        self.assertEqual(bl.pair_key_id(psk).hex(), v["pair_key_id"])
        # and the two roles' tags differ
        self.assertNotEqual(f["pair_confirm_i"].payload, f["pair_confirm_r"].payload)


if __name__ == "__main__":
    unittest.main()
