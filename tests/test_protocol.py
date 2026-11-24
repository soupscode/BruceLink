"""Host tests for the Python reference, plus a C<->Python interop check.

Run with: python3 -m unittest discover -s tests -v
The interop test builds and runs tools/bl_vectors.c and verifies the
Python reference produces byte-identical results for the same inputs.
"""
import json
import os
import shlex
import shutil
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


def compiler_command(cc_env, args, nt=(os.name == "nt")):
    """The command that runs the C compiler on args, or None if there is none.

    $CC if set (it may carry arguments, e.g. "ccache gcc", as in
    tests/c/Makefile), else the first C compiler on PATH, run by the path
    found (Windows has no `cc`; MinGW gcc and clang do).
    """
    cc_env = cc_env.strip()
    if not cc_env:
        found = next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)
        return [found, *args] if found else None
    whole = shutil.which(cc_env) or (cc_env if os.path.isfile(cc_env) else None)
    if whole:
        return [whole, *args]
    if nt:
        # Windows hands a program one command line, which the program splits
        # itself. shlex.split(posix=False) would keep the quotes around a
        # path with spaces and subprocess would then escape them, so $CC
        # goes in as written and only our own arguments are quoted.
        return f"{cc_env} {subprocess.list2cmdline(args)}"
    return [*shlex.split(cc_env), *args]


def windows_argv(command_line):
    """Split a command line with Windows' own rules (CommandLineToArgvW)."""
    import ctypes
    from ctypes import wintypes

    shell32 = ctypes.WinDLL("shell32")
    kernel32 = ctypes.WinDLL("kernel32")
    shell32.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell32.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    kernel32.LocalFree.argtypes = [ctypes.c_void_p]
    n = ctypes.c_int()
    argv = shell32.CommandLineToArgvW(command_line, ctypes.byref(n))
    if not argv:
        raise ctypes.WinError()
    try:
        return [argv[i] for i in range(n.value)]
    finally:
        kernel32.LocalFree(ctypes.cast(argv, ctypes.c_void_p))


class CompilerCommandTests(unittest.TestCase):
    """How $CC becomes the command that builds the C vectors tool."""

    ARGS = ["-std=c99", r"-IC:\src dir\core", "a.c", "-o", "out.exe"]

    def test_posix_cc_with_arguments(self):
        self.assertEqual(compiler_command("ccache gcc", self.ARGS, nt=False),
                         ["ccache", "gcc", *self.ARGS])

    def test_cc_is_one_existing_path_with_spaces(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            cc = os.path.join(d, "my compilers", "cc")
            os.makedirs(os.path.dirname(cc))
            Path(cc).touch()
            for nt in (False, True):
                self.assertEqual(compiler_command(cc, self.ARGS, nt=nt), [cc, *self.ARGS])

    @unittest.skipUnless(os.name == "nt", "checks the split with Windows' CommandLineToArgvW")
    def test_windows_quoted_path_with_arguments(self):
        # Regression (PR #9 review): shlex.split(posix=False) kept the quotes,
        # so Windows could not find the compiler.
        cc = r'"C:\Program Files\LLVM\bin\clang.exe" -m64'
        cmd = compiler_command(cc, self.ARGS, nt=True)
        line = cmd if isinstance(cmd, str) else subprocess.list2cmdline(cmd)
        self.assertEqual(windows_argv(line),
                         [r"C:\Program Files\LLVM\bin\clang.exe", "-m64", *self.ARGS])


class InteropTests(unittest.TestCase):
    """Build the C vectors tool and verify Python matches it byte-for-byte."""

    @classmethod
    def setUpClass(cls):
        core = ROOT / "shared" / "brucelink_core" / "src"
        tool = ROOT / "tools" / "bl_vectors.c"
        # Windows needs the .exe suffix to run it; elsewhere it is unsuffixed.
        out = ROOT / "tests" / ("_bl_vectors.exe" if os.name == "nt" else "_bl_vectors")
        srcs = [str(tool)] + [str(p) for p in core.glob("*.c")]
        if os.environ.get("BL_SKIP_INTEROP") == "1":
            raise unittest.SkipTest("BL_SKIP_INTEROP=1")
        cmd = compiler_command(
            os.environ.get("CC", ""),
            ["-std=c99", "-O1", "-Wall", "-Werror", f"-I{core}", *srcs, "-o", str(out)],
        )
        # No compiler is a failure, not a skip.
        if cmd is None:
            raise AssertionError("no C compiler for the C vectors tool: set CC or put cc/gcc/clang on PATH")
        # A tool that does not build is a failure, not a skip: a skipped
        # interop suite hides exactly the drift it exists to catch.
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"cannot build the C vectors tool:\n{r.stderr}")
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
        self.assertEqual(f.tag, 0xA5)  # header byte 5 carries the request tag
        # ...and re-encodes to the identical bytes.
        py = bl.encode(bl.Frame(bl.WIRE_VERSION, f.type, f.flags, 7, f.payload, 0xA5), key)
        self.assertEqual(py, c_frame)
        # the tag is under the MAC
        bad = bytearray(c_frame)
        bad[5] ^= 1
        with self.assertRaises(ValueError):
            bl.decode(bytes(bad), key)

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

    def test_session_interop(self):
        """The C handshake matches ADR 0002, recomputed here from the shared secret."""
        v = self.vectors
        self.assertTrue(v["sess_established"])
        hello = bl.decode(bytes.fromhex(v["sess_hello"]))
        ack = bl.decode(bytes.fromhex(v["sess_hello_ack"]))
        auth = bl.decode(bytes.fromhex(v["sess_auth"]))
        self.assertEqual((hello.type, ack.type, auth.type),
                         (bl.MSG_HELLO, bl.MSG_HELLO_ACK, bl.MSG_AUTH))
        psk = bytes.fromhex(v["sess_psk"])
        shared = bytes.fromhex(v["sess_shared"])
        t = bl.session_transcript(hello.payload, ack.payload)
        self.assertEqual(len(t), 3 + 32 + 16 + 32 + 16)
        # confirm_P closes HELLO_ACK, confirm_C is AUTH
        self.assertEqual(bl.session_confirm(psk, t, central=False), ack.payload[49:81])
        self.assertEqual(bl.session_confirm(psk, t, central=True), auth.payload)
        # the offered range is in the transcript: change it and the tag changes
        t2 = bl.session_transcript(bytes((0, 9)) + hello.payload[2:], ack.payload)
        self.assertNotEqual(bl.session_confirm(psk, t2, central=False), ack.payload[49:81])
        c2p, p2c = bl.session_keys(shared, psk, hello.payload[34:50], ack.payload[33:49],
                                   ack.payload[0])
        ok = bl.decode(bytes.fromhex(v["sess_auth_ok"]), p2c)  # MAC checks with p2c
        self.assertEqual(ok.type, bl.MSG_AUTH_OK)
        self.assertEqual(ok.seq, 1)
        req = bl.decode(bytes.fromhex(v["sess_request"]), c2p)  # and c2p for the Flipper
        self.assertEqual((req.seq, req.tag, req.payload), (1, 0x5A, b"\x01\x02\x03"))
        with self.assertRaises(ValueError):
            bl.decode(bytes.fromhex(v["sess_request"]), p2c)


if __name__ == "__main__":
    unittest.main()
