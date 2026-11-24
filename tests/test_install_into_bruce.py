"""tools/install_into_bruce.py against a miniature fake Bruce tree."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "tools" / "install_into_bruce.py"

# The lines of Bruce 1.17's files the installer anchors on, in their real
# order (src/core/main_menu.h, main_menu.cpp, src/main.cpp,
# menu_items/ConfigMenu.cpp).
MAIN_H = (
    "#include <MenuItemInterface.h>\n"
    '#include "menu_items/BleMenu.h"\n'
    '#include "menu_items/WireToolsMenu.h"\n'
    "class MainMenu {\n"
    "public:\n"
    "    FileMenu fileMenu;\n"
    "    WifiMenu wifiMenu;\n"
    "#if !defined(LITE_VERSION)\n"
    "    LoRaMenu loraMenu;\n"
    "#endif\n"
    "};\n"
)
MAIN_CPP = (
    '#include "main_menu.h"\n'
    "MainMenu::MainMenu() {\n"
    "    _menuItems = {\n"
    "        &wifiMenu,\n"
    "        &bleMenu,\n"
    "        &configMenu,\n"
    "    };\n"
    "}\n"
)
MAIN = (
    '#include "core/main_menu.h"\n'
    "#include <globals.h>\n"
    "void setup() {\n"
    "    wakeUpScreen();\n"
    '    if (bruceConfig.startupApp != "" && !startupApp.startApp(bruceConfig.startupApp)) {\n'
    '        bruceConfig.setStartupApp("");\n'
    "    }\n"
    "}\n"
)
CONFIG = (
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
# What the previous version of the installer left in ConfigMenu.cpp.
CONFIG_OLD_INSTALL = CONFIG.replace(
    '#include "core/wifi/wifi_common.h"\n',
    '#include "core/wifi/wifi_common.h"\n'
    '#include "modules/brucelink/BruceLinkModule.h" // BruceLink\n',
).replace(
    '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n',
    '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n'
    '            {"Toggle BruceLink", [this]() { toggleBruceLink(); }   }, // BruceLink\n'
    '            {"BruceLink pairing", [this]() { bruceLinkPairingMenu(); } }, // BruceLink\n',
)


def run(bruce, *extra):
    return subprocess.run(
        [sys.executable, str(SCRIPT), str(bruce), *extra], capture_output=True, text=True)


class InstallIntoBruce(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bruce = Path(self.tmp.name) / "bruce"
        self.main_h = self.bruce / "src/core/main_menu.h"
        self.main_cpp = self.bruce / "src/core/main_menu.cpp"
        self.config = self.bruce / "src/core/menu_items/ConfigMenu.cpp"
        self.main = self.bruce / "src/main.cpp"
        self.config.parent.mkdir(parents=True)
        self.write(MAIN_H, MAIN_CPP, CONFIG)
        (self.bruce / "platformio.ini").write_text("[platformio]\n")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, h, cpp, config, eol="\n", main=MAIN):
        for path, text in ((self.main_h, h), (self.main_cpp, cpp), (self.config, config),
                           (self.main, main)):
            path.write_bytes(text.replace("\n", eol).encode())

    def snapshot(self):
        return [p.read_bytes() for p in (self.main_h, self.main_cpp, self.config, self.main)]

    def assert_pristine(self, eol="\n"):
        self.assertEqual(
            self.snapshot(),
            [t.replace("\n", eol).encode() for t in (MAIN_H, MAIN_CPP, CONFIG, MAIN)])
        self.assertFalse((self.bruce / "lib/BruceLink").exists())
        self.assertFalse((self.bruce / "src/modules/brucelink").exists())

    def assert_guarded(self, text, needle):
        """`needle` occurs once, on a line between this script's LITE_VERSION guard lines."""
        lines = text.splitlines()
        hits = [i for i, l in enumerate(lines) if needle in l]
        self.assertEqual(len(hits), 1, text)
        i = hits[0]
        self.assertEqual(lines[i - 1], "#if !defined(LITE_VERSION) // BruceLink")
        self.assertTrue(lines[i].endswith(" // BruceLink"))
        self.assertEqual(lines[i + 1], "#endif // BruceLink")
        return i

    def test_refuses_unknown_revision_without_force(self):
        r = run(self.bruce)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("--force", r.stderr)
        self.assert_pristine()

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
        self.assertTrue((mod / "BruceLinkMenu.cpp").is_file())
        self.assertTrue((mod / "BruceBackendImpl.cpp").is_file())
        self.assertFalse((mod / "PATCH.md").exists())  # docs are not installed

        h = self.main_h.read_text()
        cpp = self.main_cpp.read_text()
        # include after the last menu header, member next to wifiMenu
        inc = self.assert_guarded(h, '#include "modules/brucelink/BruceLinkMenu.h"')
        mem = self.assert_guarded(h, "BruceLinkMenu bruceLinkMenu;")
        lines = h.splitlines()
        self.assertEqual(lines[inc - 2], '#include "menu_items/WireToolsMenu.h"')
        self.assertEqual(lines[mem - 2], "    WifiMenu wifiMenu;")
        # first item on the main menu, before Wi-Fi
        entry = self.assert_guarded(cpp, "&bruceLinkMenu,")
        self.assertEqual(cpp.splitlines()[entry + 2], "        &wifiMenu,")
        # nothing in Config any more
        self.assertEqual(self.config.read_text(), CONFIG)
        # the boot hook: declared, and called right before Bruce's startup app
        main = self.main.read_text()
        self.assert_guarded(main, '#include "modules/brucelink/BruceLinkModule.h"')
        hook = self.assert_guarded(main, "bruceLinkBoot();")
        self.assertIn("startupApp.startApp", main.splitlines()[hook + 2])
        self.assertTrue((mod / "BruceLinkJobs.cpp").is_file())

        once = self.snapshot()
        r = run(self.bruce, "--force")  # second run changes nothing
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.snapshot(), once)

    def test_uninstall_restores_the_tree(self):
        self.assertEqual(run(self.bruce, "--force").returncode, 0)
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assert_pristine()

    def test_crlf_checkout_keeps_its_line_endings(self):
        self.write(MAIN_H, MAIN_CPP, CONFIG, eol="\r\n")
        self.assertEqual(run(self.bruce, "--force").returncode, 0)
        snap = self.snapshot()
        self.assertIn(b"bruceLinkBoot", snap[3])
        for raw in snap[:2] + snap[3:]:
            self.assertIn(b"BruceLink", raw)
            self.assertNotIn(b"\n", raw.replace(b"\r\n", b""))  # no bare LF introduced
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assert_pristine(eol="\r\n")

    def test_upgrade_moves_the_entries_out_of_config(self):
        """The previous installer put two entries under Config > Advanced."""
        self.write(MAIN_H, MAIN_CPP, CONFIG_OLD_INSTALL)
        r = run(self.bruce, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.config.read_text(), CONFIG)
        self.assert_guarded(self.main_cpp.read_text(), "&bruceLinkMenu,")
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assert_pristine()

    def test_uninstall_cleans_an_old_config_install(self):
        self.write(MAIN_H, MAIN_CPP, CONFIG_OLD_INSTALL)
        self.assertEqual(run(self.bruce, "--force", "--uninstall").returncode, 0)
        self.assert_pristine()

    def test_unknown_menu_layout_is_reported_not_guessed(self):
        # main_menu.h is fine, main_menu.cpp is not: neither may be written
        self.write(MAIN_H, MAIN_CPP.replace("&wifiMenu", "&radioMenu"), CONFIG_OLD_INSTALL)
        before = self.snapshot()
        r = run(self.bruce, "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("by hand", r.stderr)
        self.assertIn("&bruceLinkMenu,", r.stderr)  # says what to add
        self.assertEqual(self.snapshot(), before)  # not even the old Config lines removed
        self.assertFalse((self.bruce / "lib/BruceLink").exists())  # nothing half-installed
        self.assertFalse((self.bruce / "src/modules/brucelink").exists())

    def test_unknown_main_layout_is_reported_not_guessed(self):
        self.write(MAIN_H, MAIN_CPP, CONFIG, main=MAIN.replace("startupApp.startApp", "runStartup"))
        before = self.snapshot()
        r = run(self.bruce, "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bruceLinkBoot", r.stderr)  # says what to add
        self.assertEqual(self.snapshot(), before)  # the menu files are not touched either

    def test_without_config_menu(self):
        """ConfigMenu.cpp is only cleaned; a tree without it still installs."""
        self.config.unlink()
        r = run(self.bruce, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assert_guarded(self.main_cpp.read_text(), "&bruceLinkMenu,")


if __name__ == "__main__":
    unittest.main()
