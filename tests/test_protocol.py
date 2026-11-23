import sys
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "shared"))
from brucelink_protocol import encode, decode, crc16

class ProtocolTests(unittest.TestCase):
    def test_round_trip(self):
        for payload in (b"", b"hello", bytes(range(256)), bytes(512)):
            self.assertEqual(decode(encode(1, payload)), (1, payload))

    def test_crc_reference(self):
        self.assertEqual(crc16(b"123456789"), 0x29B1)

    def test_corruption(self):
        packet = bytearray(encode(1, b"hello"))
        packet[6] ^= 1
        with self.assertRaises(ValueError):
            decode(bytes(packet))

    def test_length_limit(self):
        with self.assertRaises(ValueError):
            encode(1, bytes(513))

    def test_truncation(self):
        with self.assertRaises(ValueError):
            decode(encode(1, b"x")[:-1])

if __name__ == "__main__":
    unittest.main()
