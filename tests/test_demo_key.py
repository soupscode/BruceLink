"""The public demo key never reaches the Flipper app (CLAUDE.md, ADR 0005).

Review 2026-11-23: nothing automated enforced it. Checks every source under
flipper/ and, when one has been built, the .fap itself.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER = REPO / "shared" / "brucelink_core" / "src" / "bl_demo_key.h"


def demo_key() -> bytes:
    text = HEADER.read_text()
    body = text[text.index("BRUCELINK_DEMO_PSK"):]
    body = body[body.index("{") + 1:body.index("}")]
    key = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    assert len(key) == 32, len(key)
    return key


class DemoKeyNotInFlipperApp(unittest.TestCase):
    def test_no_flipper_source_uses_it(self):
        hits = []
        for f in (REPO / "flipper").rglob("*"):
            if f.suffix in (".c", ".h", ".cpp", ".fam") and f.is_file():
                text = f.read_text(errors="replace")
                if "bl_demo_key" in text or "BRUCELINK_DEMO_PSK" in text:
                    hits.append(str(f.relative_to(REPO)))
        self.assertEqual(hits, [])

    def test_built_fap_does_not_contain_it(self):
        key = demo_key()
        faps = [p for p in (REPO / "flipper" / "brucelink" / "dist" / "brucelink.fap",
                            Path.home() / ".ufbt" / "build" / "brucelink.fap") if p.is_file()]
        if not faps:
            self.skipTest("no built .fap (run ufbt first)")
        for fap in faps:
            blob = fap.read_bytes()
            # the whole key, and either half (a split constant)
            for part in (key, key[:16], key[16:]):
                self.assertNotIn(part, blob, f"{fap}: demo key bytes present")

    def test_the_parser_finds_the_key(self):
        """Guards the byte check above against a header it cannot read."""
        k = demo_key()
        self.assertEqual(k[:4], bytes((0x11, 0x22, 0x33, 0x44)))


if __name__ == "__main__":
    unittest.main()
