"""tools/install_into_bruce.py against a miniature fake Bruce tree."""
import contextlib
import importlib.util
import io
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
    "    _late_setup_gpio();\n"
    "#if !defined(USE_DUMMY_TFT)\n"
    "    if (!bruceConfig.instantBoot) {\n"
    "        boot_screen_anim();\n"
    "    }\n"
    "#endif\n"
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
STICKS3_INI = (
    "[env:m5stack-sticks3]\n"
    "build_flags =\n"
    "\t${env.build_flags}\n"
    "\n"
    "\t-Iboards/m5stack-sticks3\n"
    "\t-DUSB_as_HID=1  ;uncomment to enable\n"
    "\n"
    "lib_ignore =\n"
    "\tTFT_eSPI\n"
)
# The Bruce files the RAM trims insert guarded lines into, cut to their anchors.
TRIM_FILES = {
    "src/core/massStorage.h": (
        "#ifndef __MASS_STORAGE_H__\n#define __MASS_STORAGE_H__\n\n#include <globals.h>\n"
        "#if defined(SOC_USB_OTG_SUPPORTED)\n#include <USBMSC.h>\n#endif\n#endif\n"),
    "src/modules/wifi/wifi_recover.cpp": (
        "#ifndef LITE_VERSION\n#include <string.h>\n#include <string>\n\n"
        "static IRAM_ATTR void sha1_transform(void) {}\n#endif\n"),
    "src/modules/wifi/netcut.cpp": (
        '#include "netcut.h"\nstatic void _activeLoop();\n'
        "static NetCutDevice s_devices[NETCUT_MAX_DEVICES];\nstatic int s_deviceCount = 0;\n"),
    "src/modules/rf/protocols/rf_stream_memory.c": (
        "#include <stdlib.h>\ntypedef struct {\n    void *p;\n    size_t n;\n} Allocation;\n"
        "static Allocation allocated[512];\nstatic size_t used, total;\n"),
    "src/core/wifi/webInterface.h": "#pragma once\n#include <ESPmDNS.h>\nextern bool mdnsRunning;\n",
    "src/core/wifi/webInterface.cpp": (
        "void stopWebUi() {\n    if (mdnsRunning) {\n        MDNS.end();\n        mdnsRunning = false;\n"
        "    }\n}\nstatic bool startMdnsResponder() {\n    if (!MDNS.begin(host)) {\n"
        "        return false;\n    }\n    return true;\n}\n"),
}
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
        self.ini = self.bruce / "boards/m5stack-sticks3/m5stack-sticks3.ini"
        self.config.parent.mkdir(parents=True)
        self.ini.parent.mkdir(parents=True)
        self.trim = {rel: self.bruce / rel for rel in TRIM_FILES}
        for p in self.trim.values():
            p.parent.mkdir(parents=True, exist_ok=True)
        self.write(MAIN_H, MAIN_CPP, CONFIG)
        (self.bruce / "platformio.ini").write_text("[platformio]\n")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, h, cpp, config, eol="\n", main=MAIN, ini=STICKS3_INI):
        files = [(self.main_h, h), (self.main_cpp, cpp), (self.config, config),
                 (self.main, main), (self.ini, ini)]
        files += [(self.trim[rel], text) for rel, text in TRIM_FILES.items()]
        for path, text in files:
            path.write_bytes(text.replace("\n", eol).encode())

    def snapshot(self):
        paths = [self.main_h, self.main_cpp, self.config, self.main, self.ini, *self.trim.values()]
        return [p.read_bytes() for p in paths]

    def assert_pristine(self, eol="\n"):
        self.assertEqual(
            self.snapshot(),
            [t.replace("\n", eol).encode()
             for t in (MAIN_H, MAIN_CPP, CONFIG, MAIN, STICKS3_INI, *TRIM_FILES.values())])
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

    def assert_trims(self):
        """Each RAM trim: guarded by BRUCELINK_RAM_TRIM, every added line marked,
        and the Bruce lines themselves untouched."""
        for rel, orig in TRIM_FILES.items():
            text = self.trim[rel].read_text()
            added = [l for l in text.splitlines() if l.endswith(" // BruceLink")]
            self.assertTrue(added, rel)
            self.assertTrue(any("BRUCELINK_RAM_TRIM" in l for l in added), rel)
            kept = [l for l in text.splitlines() if not l.endswith(" // BruceLink")]
            self.assertEqual(kept, orig.splitlines(), rel)
        web = self.trim["src/core/wifi/webInterface.cpp"].read_text().splitlines()
        i = web.index("        MDNS.end();")
        self.assertEqual(web[i - 1], "#if !defined(BRUCELINK_RAM_TRIM) // BruceLink")
        self.assertEqual(web[i + 1], "#endif // BruceLink")
        j = web.index("    if (!MDNS.begin(host)) {")
        self.assertEqual(web[j - 1], "#else // BruceLink")
        self.assertEqual(web[j + 1], "#endif // BruceLink")
        net = self.trim["src/modules/wifi/netcut.cpp"].read_text().splitlines()
        k = net.index("static NetCutDevice s_devices[NETCUT_MAX_DEVICES];")
        self.assertIn("#define s_devices (*s_devices_ps)", net[k - 2])
        self.assertEqual(net[k + 1], "#if defined(BRUCELINK_RAM_TRIM) // BruceLink")

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
        # the early hook: after the input setup, before the boot animation
        early = self.assert_guarded(main, "bruceLinkBootEarly();")
        self.assertEqual(main.splitlines()[early - 2], "    _late_setup_gpio();")
        self.assertIn("USE_DUMMY_TFT", main.splitlines()[early + 2])
        self.assertTrue((mod / "BruceLinkJobs.cpp").is_file())
        # the NimBLE-in-PSRAM forced include, right after the inherited flags,
        # pointing at a header the library copy has
        ini = self.ini.read_text().splitlines()
        i = ini.index("\t${env.build_flags}")
        self.assertEqual(ini[i + 1], "\t-DBRUCELINK_RAM_TRIM=1 ; BruceLink")
        self.assertEqual(ini[i + 2], "\t-I lib/BruceLink ; BruceLink")
        self.assertEqual(ini[i + 3], "\t-include bl_sticks3_ram.h ; BruceLink")
        self.assertEqual(sum("bl_sticks3_ram" in l for l in ini), 1)
        self.assertTrue((lib / "bl_sticks3_ram.h").is_file())
        k = ini.index("lib_ignore =")
        self.assertEqual(ini[k + 1], "\tESPmDNS ; BruceLink")
        self.assertEqual(ini[k + 2], "\tTFT_eSPI")
        self.assert_trims()

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

    def test_unknown_board_ini_is_reported_not_guessed(self):
        self.write(MAIN_H, MAIN_CPP, CONFIG, ini=STICKS3_INI.replace("${env.build_flags}", "${common.flags}"))
        before = self.snapshot()
        r = run(self.bruce, "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bl_sticks3_ram.h", r.stderr)  # says what to add
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.bruce / "lib/BruceLink").exists())

    def test_unknown_trim_file_is_reported_not_guessed(self):
        net = self.trim["src/modules/wifi/netcut.cpp"]
        net.write_text(TRIM_FILES["src/modules/wifi/netcut.cpp"].replace("[NETCUT_MAX_DEVICES]", "[128]"))
        before = self.snapshot()
        r = run(self.bruce, "--force")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("s_devices_ps", r.stderr)  # says what to add
        self.assertEqual(self.snapshot(), before)

    def test_without_config_menu(self):
        """ConfigMenu.cpp is only cleaned; a tree without it still installs."""
        self.config.unlink()
        r = run(self.bruce, "--force")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assert_guarded(self.main_cpp.read_text(), "&bruceLinkMenu,")

    def test_name_collision_writes_nothing(self):
        """Review 2026-11-23: Bruce's files were patched before the file
        lists were checked, so a collision failed half-installed."""
        spec = importlib.util.spec_from_file_location("bl_installer", SCRIPT)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        # the same directory twice: every name collides
        mod.LIB_SOURCES = mod.LIB_SOURCES + mod.LIB_SOURCES[:1]
        before = self.snapshot()
        argv = sys.argv
        sys.argv = ["install_into_bruce.py", str(self.bruce), "--force"]
        try:
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()), \
                    contextlib.redirect_stdout(io.StringIO()):
                mod.main()
        finally:
            sys.argv = argv
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.bruce / "lib/BruceLink").exists())
        self.assertFalse((self.bruce / "src/modules/brucelink").exists())


if __name__ == "__main__":
    unittest.main()
