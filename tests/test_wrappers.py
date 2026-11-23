"""The device builds pull the shared core in through generated wrapper
sources (tools/gen_wrappers.py). Fail if they are stale, so a new core file
cannot be host-tested but silently missing from a device build."""
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WrapperTests(unittest.TestCase):
    def test_wrappers_up_to_date(self):
        r = subprocess.run(
            [sys.executable, str(ROOT / "tools" / "gen_wrappers.py"), "--check"],
            capture_output=True, text=True,
        )
        self.assertEqual(r.returncode, 0, "run tools/gen_wrappers.py\n" + r.stdout)

    def test_no_symlinks_in_repo(self):
        tracked = subprocess.check_output(
            ["git", "-C", str(ROOT), "ls-files", "-s"], text=True
        ).splitlines()
        links = [line.split("\t", 1)[1] for line in tracked if line.startswith("120000")]
        self.assertEqual(links, [], "symlinks break Windows checkouts")


if __name__ == "__main__":
    unittest.main()
