#!/usr/bin/env python3
"""Install the BruceLink bridge into a Bruce firmware checkout.

    python3 tools/install_into_bruce.py /path/to/bruce-firmware

What it does (and nothing else):
  1. copies the bridge library to   <bruce>/lib/BruceLink/
       esp32/lib/BruceLink/*          (bridge core + NimBLE central transport)
       shared/brucelink_core/src/*    (shared protocol core)
  2. copies the Bruce glue to       <bruce>/src/modules/brucelink/
  3. adds "BruceLink" to Bruce's main menu, first, before Wi-Fi:
       <bruce>/src/core/main_menu.h    include + member
       <bruce>/src/core/main_menu.cpp  entry in the item list
  4. calls bruceLinkBootEarly() after the input task starts and
     bruceLinkBoot() at the end of setup() in <bruce>/src/main.cpp: they run
     a pending BruceLink job and bring the link back after it (ADR 0006)
  5. adds two build flags to the StickS3 environment
     (<bruce>/boards/m5stack-sticks3/m5stack-sticks3.ini): lib/BruceLink on
     the include path and a forced include of bl_nimble_psram.h, which puts
     NimBLE's host buffers in PSRAM (about 16 kB less internal RAM at BLE
     start)
     Every added line ends with "// BruceLink" (C++) or "; BruceLink" (ini).
     Older versions of this script put two entries under Config > Advanced
     (ConfigMenu.cpp); re-running removes those.

It is idempotent (safe to run again after updating BruceLink) and refuses
to touch a tree that does not look like the Bruce version it was written
against, unless --force is given. `--uninstall` reverses all of it.

Then build Bruce as usual:  pio run -e m5stack-sticks3
SPDX-License-Identifier: MIT
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PINNED_BRUCE = "2b6ba4d"  # Bruce 1.17

LIB_SOURCES = [
    (REPO / "esp32" / "lib" / "BruceLink", ("*.h", "*.cpp")),
    (REPO / "shared" / "brucelink_core" / "src", ("*.h", "*.c")),
]
MODULE_SOURCE = REPO / "esp32" / "src" / "modules" / "brucelink"
MODULE_PATTERNS = ("*.h", "*.cpp")

MARKER = " // BruceLink"
INI_MARKER = " ; BruceLink"  # PlatformIO strips "; ..." from build_flags lines
GUARD = "#if !defined(LITE_VERSION)" + MARKER + "\n"  # the module is not built in LITE builds
END = "#endif" + MARKER + "\n"

MAIN_MENU_H = Path("src/core/main_menu.h")
MAIN_MENU_CPP = Path("src/core/main_menu.cpp")
BRUCE_MAIN = Path("src/main.cpp")
CONFIG_MENU = Path("src/core/menu_items/ConfigMenu.cpp")  # older installs only: cleaned
STICKS3_INI = Path("boards/m5stack-sticks3/m5stack-sticks3.ini")

# (file, [(anchor line, "before" | "after", lines to add)]). Each anchor must
# occur exactly once in the unpatched file.
PATCHES = [
    (MAIN_MENU_H, [
        ('#include "menu_items/WireToolsMenu.h"\n', "after",
         GUARD + '#include "modules/brucelink/BruceLinkMenu.h"' + MARKER + "\n" + END),
        ("    WifiMenu wifiMenu;\n", "after",
         GUARD + "    BruceLinkMenu bruceLinkMenu;" + MARKER + "\n" + END),
    ]),
    (MAIN_MENU_CPP, [
        ("        &wifiMenu,\n", "before",
         GUARD + "        &bruceLinkMenu," + MARKER + "\n" + END),
    ]),
    (BRUCE_MAIN, [
        ('#include "core/main_menu.h"\n', "after",
         GUARD + '#include "modules/brucelink/BruceLinkModule.h"' + MARKER + "\n" + END),
        ("    _late_setup_gpio();\n", "after",
         GUARD + "    bruceLinkBootEarly(); // a pending job runs here, before the boot animation"
         + MARKER + "\n" + END),
        ('    if (bruceConfig.startupApp != "" && !startupApp.startApp(bruceConfig.startupApp)) {\n',
         "before",
         GUARD + "    bruceLinkBoot(); // after a job: turns BruceLink back on for its result" + MARKER
         + "\n" + END),
    ]),
    (STICKS3_INI, [
        # -include takes the path as given, against each compile's own working
        # directory; -I paths are made absolute by PlatformIO. So the header
        # is named bare and found through the include path.
        ("\t${env.build_flags}\n", "after",
         "\t-I lib/BruceLink" + INI_MARKER + "\n"
         "\t-include bl_nimble_psram.h" + INI_MARKER + "\n"),
    ]),
    (CONFIG_MENU, []),
]

LIBRARY_JSON = """{
  "name": "BruceLink",
  "version": "0.1.0",
  "description": "BruceLink bridge core, NimBLE central transport and shared protocol core. Installed by BruceLink's tools/install_into_bruce.py; do not edit here.",
  "frameworks": "arduino",
  "platforms": "espressif32",
  "dependencies": [{"name": "NimBLE-Arduino"}],
  "build": {"srcDir": ".", "includeDir": ".", "libLDFMode": "chain+"}
}
"""


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def bruce_revision(bruce):
    try:
        out = subprocess.run(
            ["git", "-C", str(bruce), "rev-parse", "--short=7", "HEAD"],
            capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def collect(sources, dest):
    """The files matched in sources, by name (flat); fails on a collision.
    Writes nothing, so it can run before anything is changed."""
    names = {}
    for src_dir, patterns in sources:
        for pattern in patterns:
            for f in sorted(src_dir.glob(pattern)):
                if f.name in names:
                    fail(f"{f} and {names[f.name]} would collide in {dest}")
                names[f.name] = f
    return names


def copy_tree(names, dest):
    """Replace dest with exactly the files in names (from collect)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    for name, f in names.items():
        shutil.copyfile(f, dest / name)
    return len(names)


def read_text(path):
    raw = path.read_bytes()
    crlf = b"\r\n" in raw  # keep the checkout's line endings (Windows autocrlf)
    return raw.decode("utf-8").replace("\r\n", "\n"), crlf


def unpatched(text):
    """The text without any line this script ever added (they all end with
    MARKER or INI_MARKER), so re-running never duplicates and old layouts
    are removed."""
    return "".join(
        line for line in text.splitlines(keepends=True)
        if not line.rstrip("\n").endswith((MARKER, INI_MARKER))
    )


def plan_patches(bruce, uninstall):
    """Work out every file's new text first; fail before writing anything if
    an anchor is missing. Returns [(path, old_text, new_text, crlf)]."""
    plan = []
    for rel, edits in PATCHES:
        path = bruce / rel
        if not path.is_file():
            if edits:
                fail(f"{path} is missing; this does not look like Bruce {PINNED_BRUCE}")
            continue
        text, crlf = read_text(path)
        new = unpatched(text)
        if not uninstall:
            for anchor, where, lines in edits:
                if new.count(anchor) != 1:
                    fail(f"{path}: expected exactly one line\n    {anchor.strip()}\n"
                         f"  (found {new.count(anchor)}). This Bruce version differs from "
                         f"{PINNED_BRUCE}; add these lines {where} it by hand:\n"
                         + "".join("    " + l + "\n" for l in lines.splitlines()))
                new = new.replace(anchor, lines + anchor if where == "before" else anchor + lines)
        plan.append((path, text, new, crlf))
    return plan


def apply_patches(plan):
    changed = []
    for path, text, new, crlf in plan:
        if new != text:
            out = new.replace("\n", "\r\n") if crlf else new
            path.write_bytes(out.encode("utf-8"))
            changed.append(path)
    return changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bruce", type=Path, help="path to a Bruce firmware checkout")
    ap.add_argument("--force", action="store_true",
                    help=f"proceed even if the checkout is not Bruce {PINNED_BRUCE}")
    ap.add_argument("--uninstall", action="store_true", help="remove BruceLink from the checkout")
    args = ap.parse_args()

    bruce = args.bruce.resolve()
    if not (bruce / "platformio.ini").is_file() or not (bruce / MAIN_MENU_CPP).is_file():
        fail(f"{bruce} does not look like a Bruce firmware checkout")

    rev = bruce_revision(bruce)
    if rev != PINNED_BRUCE:
        msg = (f"{bruce} is at {rev or 'an unknown revision'}, not Bruce 1.17 "
               f"({PINNED_BRUCE}), which BruceLink is written against")
        if not args.force:
            fail(msg + ". Check out that revision or pass --force.")
        print("warning: " + msg)

    lib_dest = bruce / "lib" / "BruceLink"
    mod_dest = bruce / "src" / "modules" / "brucelink"

    if args.uninstall:
        for d in (lib_dest, mod_dest):
            if d.exists():
                shutil.rmtree(d)
        apply_patches(plan_patches(bruce, uninstall=True))
        print(f"BruceLink removed from {bruce}")
        return

    # Every check before the first write (the file lists, then Bruce's
    # files), so a failed install leaves the tree untouched.
    lib_files = collect(LIB_SOURCES, lib_dest)
    mod_files = collect([(MODULE_SOURCE, MODULE_PATTERNS)], mod_dest)
    plan = plan_patches(bruce, uninstall=False)
    changed = apply_patches(plan)
    n_lib = copy_tree(lib_files, lib_dest)
    (lib_dest / "library.json").write_text(LIBRARY_JSON, encoding="utf-8", newline="\n")
    n_mod = copy_tree(mod_files, mod_dest)

    print(f"BruceLink installed into {bruce}")
    print(f"  lib/BruceLink/            {n_lib} files + library.json")
    print(f"  src/modules/brucelink/    {n_mod} files")
    for path in changed:
        print(f"  {path.relative_to(bruce)}  updated")
    if not changed:
        print("  main menu entry already present")
    print("Build:  pio run -e m5stack-sticks3")
    print("On the device:  main menu > BruceLink > Pair with Flipper, then Turn on")


if __name__ == "__main__":
    main()
