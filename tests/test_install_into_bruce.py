"""tools/install_into_bruce.py against a miniature fake Bruce tree."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tools" / "install_into_bruce.py"

MENU = (
    '#include "ConfigMenu.h"\n'
    '#include "core/wifi/wifi_common.h"\n'
    "void ConfigMenu::advancedMenu() {\n"
    "        std::vector<Option> localOptions = {\n"
    "#if !defined(LITE_VERSION)\n"
    '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n'
    "#endif\n"
    "        };\n"
    "}\n"
)


def run(bruce, *extra):
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(bruce), *extra], capture_output=True, text=True)


class InstallIntoBruce(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bruce = Path(self.tmp.name) / "bruce"
        menu = self.bruce / "src/core/menu_items/ConfigMenu.cpp"
        menu.parent.mkdir(parents=True)
        menu.write_bytes(MENU.encode())
        (self.bruce / "platformio.ini").write_text("[platformio]\n")
        self.menu = menu

    def tearDown(self):
        self.tmp.cleanup()

    def test_refuses_unknown_revision_without_force(self):
        r = run(self.bruce)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--force", r.stderr)
        self.assertEqual(self.menu.read_bytes(), MENU.encode())  # untouched
        self.assertFalse((self.bruce / "lib/BruceLink").exists())

    def test_install_is_complete_and_idempotent(self):
        r = run(self.bruce, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        lib = self.bruce / "lib/BruceLink"
        mod = self.bruce / "src/modules/brucelink"
        # every bridge + core source is there, flat, once
        want = {f.name for f in (REPO / "esp32/lib/BruceLink").glob("*.h")}
        want |= {f.name for f in (REPO / "esp32/lib/BruceLink").glob("*.cpp")}
        want |= {f.name for f in (REPO / "shared/brucelink_core/src").glob("*.h")}
        want |= {f.name for f in (REPO / "shared/brucelink_core/src").glob("*.c")}
        self.assertEqual({f.name for f in lib.iterdir()}, want | {"library.json"})
        self.assertTrue((mod / "BruceLinkModule.cpp").is_file())
        self.assertTrue((mod / "BruceBackendImpl.cpp").is_file())
        self.assertFalse((mod / "PATCH.md").exists())  # docs are not installed
        once = self.menu.read_text()
        self.assertEqual(once.count("toggleBruceLink"), 1)
        self.assertEqual(once.count("bruceLinkPairingMenu"), 1)
        self.assertEqual(once.count("BruceLinkModule.h"), 1)
        # the entry sits inside the same LITE_VERSION guard as the BLE API toggle
        self.assertLess(once.index("enableBLEAPI"), once.index("toggleBruceLink"))
        self.assertLess(once.index("toggleBruceLink"), once.index("#endif"))
        self.assertLess(once.index("bruceLinkPairingMenu"), once.index("#endif"))
        # second run changes nothing
        r = run(self.bruce, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.menu.read_text(), once)

    def test_uninstall_restores_the_tree(self):
        self.assertEqual(run(self.bruce, "--force").returncode, 0)
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assertEqual(self.menu.read_bytes(), MENU.encode())
        self.assertFalse((self.bruce / "lib/BruceLink").exists())
        self.assertFalse((self.bruce / "src/modules/brucelink").exists())

    def test_crlf_checkout_keeps_its_line_endings(self):
        self.menu.write_bytes(MENU.replace("\n", "\r\n").encode())
        self.assertEqual(run(self.bruce, "--force").returncode, 0)
        raw = self.menu.read_bytes()
        self.assertIn(b"toggleBruceLink", raw)
        self.assertNotIn(b"\n", raw.replace(b"\r\n", b""))  # no bare LF introduced
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assertEqual(self.menu.read_bytes(), MENU.replace("\n", "\r\n").encode())

    def test_upgrade_from_a_single_entry_install(self):
        """An older install added only the toggle; re-running adds pairing once."""
        old = MENU.replace(
            '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n',
            '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n'
            '            {"Toggle BruceLink", [this]() { toggleBruceLink(); }   }, // BruceLink\n',
        ).replace(
            '#include "core/wifi/wifi_common.h"\n',
            '#include "core/wifi/wifi_common.h"\n'
            '#include "modules/brucelink/BruceLinkModule.h" // BruceLink\n',
        )
        self.menu.write_text(old)
        self.assertEqual(run(self.bruce, "--force").returncode, 0)
        text = self.menu.read_text()
        self.assertEqual(text.count("toggleBruceLink"), 1)
        self.assertEqual(text.count("bruceLinkPairingMenu"), 1)
        self.assertEqual(text.count("BruceLinkModule.h"), 1)
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assertEqual(self.menu.read_bytes(), MENU.encode())

    def test_unknown_menu_layout_is_reported_not_guessed(self):
        self.menu.write_text(MENU.replace("Toggle BLE API", "Something else"))
        r = run(self.bruce, "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("by hand", r.stderr)
        self.assertNotIn("toggleBruceLink", self.menu.read_text())
        self.assertFalse((self.bruce / "lib/BruceLink").exists())  # nothing half-installed
        self.assertFalse((self.bruce / "src/modules/brucelink").exists())


if __name__ == "__main__":
    unittest.main()
