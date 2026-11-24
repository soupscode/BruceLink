"""Host tests for scripts/hil/hil.py: the parts that read device output.

No device and no pyserial needed: the serial port is faked, and the parsers
take bytes. Run with: python3 -m unittest discover -s tests -v
"""
import hashlib
import importlib.util
import struct
import types
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("hil", ROOT / "scripts" / "hil" / "hil.py")
hil = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hil)

SHT_PROGBITS, SHT_STRTAB, SHT_NOBITS = 1, 3, 8


def make_elf(sections):
    """A minimal little-endian ELF32 holding (name, type, bytes) sections."""
    secs = list(sections) + [(".shstrtab", SHT_STRTAB, None)]
    strtab, name_offs = b"\0", []
    for name, _, _ in secs:
        name_offs.append(len(strtab))
        strtab += name.encode() + b"\0"
    body, headers = b"", [bytes(40)]  # section 0 is the null section
    for (name, typ, data), name_off in zip(secs, name_offs):
        data = strtab if data is None else data
        off = 52 + len(body)
        if typ != SHT_NOBITS:
            body += data
        headers.append(struct.pack("<10I", name_off, typ, 0, 0, off, len(data), 0, 0, 1, 0))
    shoff = 52 + len(body)
    ehdr = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9) + struct.pack(
        "<HHIIIIIHHHHHH", 1, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, len(headers), len(headers) - 1)
    return ehdr + body + b"".join(headers)


class FapCodeDigestTests(unittest.TestCase):
    def fap(self, text=b"\x01\x02code", debuglink=b"brucelink_d.elf\0\0\0\0\xaa\xbb\xcc\xdd"):
        return make_elf([(".text", SHT_PROGBITS, text),
                         (".gnu_debuglink", SHT_PROGBITS, debuglink),
                         (".bss", SHT_NOBITS, bytes(64))])

    def test_debuglink_is_ignored(self):
        # Two builds of the same source differ only in .gnu_debuglink.
        a, b = self.fap(), self.fap(debuglink=b"other_build.elf\0\0\0\0\x11\x22\x33\x44")
        self.assertNotEqual(a, b)
        self.assertEqual(hil.fap_code_digest(a), hil.fap_code_digest(b))

    def test_code_change_is_seen(self):
        self.assertNotEqual(hil.fap_code_digest(self.fap()),
                            hil.fap_code_digest(self.fap(text=b"\x01\x02codf")))

    def test_rejects_non_elf32(self):
        with self.assertRaises(ValueError):
            hil.fap_code_digest(b"PK\x03\x04" + bytes(60))
        elf64 = bytearray(self.fap())
        elf64[4] = 2
        with self.assertRaises(ValueError):
            hil.fap_code_digest(bytes(elf64))


def part_entry(name, ptype, sub, off, size, flags=0):
    return b"\xaa\x50" + struct.pack("<BBII", ptype, sub, off, size) + name.ljust(16, b"\0") \
        + struct.pack("<I", flags)


class PartitionTableTests(unittest.TestCase):
    def test_entries_until_the_md5_entry(self):
        table = (part_entry(b"nvs", 1, 2, 0x9000, 0x5000)
                 + part_entry(b"app0", 0, 0x10, 0x10000, 0x4E0000)
                 + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(b"x").digest())
        table += b"\xff" * (0x1000 - len(table))
        self.assertEqual(hil.parse_partitions(table), [
            {"name": "nvs", "type": 1, "subtype": 2, "offset": 0x9000, "size": 0x5000},
            {"name": "app0", "type": 0, "subtype": 0x10, "offset": 0x10000, "size": 0x4E0000},
        ])

    def test_erased_table_is_empty(self):
        self.assertEqual(hil.parse_partitions(b"\xff" * 0x1000), [])


class AppDescTests(unittest.TestCase):
    def image(self, magic=0xABCD5432):
        desc = struct.pack("<II", magic, 0) + bytes(8)
        desc += b"3.3.9".ljust(32, b"\0") + b"Bruce".ljust(32, b"\0")
        desc += b"12:34:56".ljust(16, b"\0") + b"Nov 24 2026".ljust(16, b"\0")
        desc += b"v5.5.4".ljust(32, b"\0") + bytes(range(32))
        return bytes(24 + 8) + desc.ljust(256, b"\0")

    def test_fields(self):
        self.assertEqual(hil.app_desc(self.image()), {
            "version": "3.3.9", "project_name": "Bruce", "time": "12:34:56",
            "date": "Nov 24 2026", "idf_ver": "v5.5.4", "elf_sha256": bytes(range(32)).hex(),
        })

    def test_no_descriptor(self):
        self.assertIsNone(hil.app_desc(self.image(magic=0xFFFFFFFF)))
        self.assertIsNone(hil.app_desc(self.image()[:100]))


class VerifyFlashTests(unittest.TestCase):
    def test_esptool5_match(self):
        self.assertTrue(hil.verify_flash_ok(
            0, "Verifying 0x800000 (8388608) bytes at 0x00000000 in flash against 'x.bin'...\n"
               "Verification successful (digest matched).\n"))

    def test_esptool4_match(self):
        # Regression: the first version accepted only esptool 5's wording.
        self.assertTrue(hil.verify_flash_ok(0, "-- verify OK (digest matched)\n"))

    def test_mismatch(self):
        self.assertFalse(hil.verify_flash_ok(2, "Verification failed (digest mismatch).\n"))
        self.assertFalse(hil.verify_flash_ok(2, "-- verify FAILED (digest mismatch)\n"))

    def test_failed_run(self):
        self.assertFalse(hil.verify_flash_ok(0, "Connecting...\n"))
        self.assertFalse(hil.verify_flash_ok(1, "Verification successful (digest matched).\n"))

    def test_path_with_mismatch_in_it(self):
        # Review finding: esptool echoes the file path.
        self.assertTrue(hil.verify_flash_ok(
            0, "Verifying 0x800000 (8388608) bytes at 0x00000000 in flash against "
               "'/backups/mismatch-hunt/sticks3-full.bin'...\n"
               "Verification successful (digest matched).\n"))


class BootBannerTests(unittest.TestCase):
    def test_reset_seen(self):
        lines = ["ESP-ROM:esp32s3-20210327", "Build:Mar 27 2021",
                 "rst:0x15 (USB_UART_CHIP_RESET),boot:0x2b (SPI_FAST_FLASH_BOOT)"]
        self.assertEqual(hil.boot_banner(lines), [lines[0], lines[2]])

    def test_quiet_or_running(self):
        self.assertEqual(hil.boot_banner([]), [])
        self.assertEqual(hil.boot_banner(["[BruceLink] module: BLE up", "free: 181063"]), [])


class BackupRootTests(unittest.TestCase):
    def test_refused_inside_the_repository(self):
        # Backups hold pairing keys (review finding: they defaulted to the checkout).
        for d in (hil.REPO, hil.REPO / "hil-logs", hil.REPO / "backups" / "new"):
            with self.assertRaises(SystemExit):
                hil.backup_root(str(d))

    def test_outside_any_checkout(self):
        import tempfile
        with tempfile.TemporaryDirectory() as t:
            if hil.in_git_checkout(t):
                self.skipTest("the temp folder is inside a git checkout here")
            d = Path(t) / "backups" / "new"
            self.assertEqual(hil.backup_root(str(d)), d)
            self.assertTrue(d.is_dir())


class LineRecorder:
    """A pyserial Serial stand-in that records the DTR/RTS line states the
    device sees. POSIX: the kernel raises both lines at open, then pyserial
    applies DTR, then RTS. Windows: pyserial applies both at once."""

    def __init__(self, posix):
        self.posix, self.states, self.is_open = posix, [], False
        self._dtr = self._rts = True  # pyserial's defaults

    def _set(self, name, value):
        setattr(self, name, value)
        if self.is_open:
            self.states.append((self._dtr, self._rts))

    dtr = property(lambda s: s._dtr, lambda s, v: s._set("_dtr", v))
    rts = property(lambda s: s._rts, lambda s, v: s._set("_rts", v))

    def open(self):
        want = (self._dtr, self._rts)
        if self.posix:
            self._dtr = self._rts = True
            self.states.append((True, True))
            self._dtr = want[0]
            self.states.append((self._dtr, self._rts))
            self._rts = want[1]
            self.states.append((self._dtr, self._rts))
        else:
            self._dtr, self._rts = want
            self.states.append(want)
        self.is_open = True


class OpenNoResetTests(unittest.TestCase):
    # The ESP32-S3 USB-Serial/JTAG resets the chip while RTS is on and DTR off.
    RESET = (False, True)

    def states(self, posix):
        rec = LineRecorder(posix)
        with mock.patch.object(hil, "serial", types.SimpleNamespace(Serial=lambda: rec)):
            hil.open_no_reset("PORT", posix=posix)
        return rec.states

    def test_posix_never_passes_through_reset(self):
        # Review finding: DTR off + RTS off before open went on/on, off/on, off/off.
        states = self.states(posix=True)
        self.assertNotIn(self.RESET, states)
        self.assertEqual(states[-1], (False, False))

    def test_windows_unchanged(self):
        # The order verified on the StickS3: both lines off at once.
        self.assertEqual(self.states(posix=False), [(False, False)])


class FakeSerial:
    """Replays a device's reply `chunk` bytes at a time."""

    def __init__(self, reply, chunk=7):
        self.reply, self.chunk, self.written = reply, chunk, b""

    def reset_input_buffer(self):
        pass

    def write(self, data):
        self.written += data

    def read(self, n):
        out, self.reply = self.reply[: min(n, self.chunk)], self.reply[min(n, self.chunk):]
        return out


def fake_cli(reply, chunk=7):
    cli = hil.FlipperCli.__new__(hil.FlipperCli)  # no port opened
    cli.s = FakeSerial(reply, chunk)
    return cli


def storage_read_reply(path, data):
    # As the Flipper sends it: echo, size line, the bytes, CR/LF, the prompt.
    return b"storage read %s\r\nSize: %d\r\n" % (path, len(data)) + data + b"\r\n\r\n>: "


class FlipperCliTests(unittest.TestCase):
    def test_read_file_returns_exact_bytes(self):
        # The file's own bytes may contain the prompt and CR/LF.
        data = b"\x00\x01\r\n>: Size: 9\r\n\xff" + bytes(range(200))
        cli = fake_cli(storage_read_reply(b"/ext/a.bin", data))
        self.assertEqual(cli.read_file("/ext/a.bin", timeout=2), data)
        self.assertEqual(cli.s.written, b"storage read /ext/a.bin\r")

    def test_read_file_prompt_split_across_reads(self):
        # Review finding: when the read that completes the size line also
        # holds the whole file and part of the prompt, that part was dropped
        # and the prompt never seen. Every split of the trailer, 256-byte reads.
        path = b"/ext/apps_data/brucelink/f"
        head = len(b"storage read %s\r\nSize: 190\r\n" % path)
        for cut in range(1, 7):
            data = bytes(256 - head - cut)
            cli = fake_cli(storage_read_reply(path, data), chunk=256)
            self.assertEqual(cli.read_file(path.decode(), timeout=2), data, f"cut {cut}")

    def test_read_file_missing(self):
        cli = fake_cli(b"storage read /ext/nope\r\nStorage error: file/dir not exist\r\n\r\n>: ")
        with self.assertRaises(FileNotFoundError):
            cli.read_file("/ext/nope", timeout=2)

    def test_listdir(self):
        cli = fake_cli(b"storage list /ext/apps_data/brucelink\r\n"
                       b"\t[D] captures\r\n\t[F] pairing.key 96b\r\n\t[F] settings.txt 12b\r\n>: ")
        self.assertEqual(cli.listdir("/ext/apps_data/brucelink"),
                         [("D", "captures", None), ("F", "pairing.key", 96), ("F", "settings.txt", 12)])

    def test_md5_and_info(self):
        md5 = "0123456789abcdef0123456789abcdef"
        self.assertEqual(fake_cli(b"storage md5 /ext/x\r\n" + md5.encode() + b"\r\n>: ").md5("/ext/x"), md5)
        info = fake_cli(b"device_info\r\nfirmware_version     : mntm-012\r\n"
                        b"firmware_api_major   : 87\r\n>: ").info()
        self.assertEqual(info["firmware_version"], "mntm-012")
        self.assertEqual(info["firmware_api_major"], "87")


class StickS3CliTests(unittest.TestCase):
    def test_only_read_only_commands(self):
        # Refused before any port is opened (so this needs no device).
        for cmd in ("reboot", "wifi on", "info; reboot", ""):
            with self.assertRaises(ValueError):
                hil.sticks3_cli("NO_SUCH_PORT", cmd)


if __name__ == "__main__":
    unittest.main()
