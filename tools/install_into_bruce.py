#!/usr/bin/env python3
"""Install the BruceLink bridge into a Bruce firmware checkout.

    python3 tools/install_into_bruce.py /path/to/bruce-firmware

What it does (and nothing else):
  1. copies the bridge library to   <bruce>/lib/BruceLink/
       esp32/lib/BruceLink/*          (bridge core + NimBLE central transport)
       shared/brucelink_core/src/*    (shared protocol core)
  2. copies the Bruce glue to       <bruce>/src/modules/brucelink/
  3. adds one menu entry to         <bruce>/src/core/menu_items/ConfigMenu.cpp
       Config > Advanced > "Toggle BruceLink", next to "Toggle BLE API"

It is idempotent (safe to run again after updating BruceLink) and refuses
to touch a tree that does not look like the Bruce version it was written
against, unless --force is given. `--uninstall` reverses all three steps.

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

MENU_FILE = Path("src/core/menu_items/ConfigMenu.cpp")
MENU_INCLUDE_ANCHOR = '#include "core/wifi/wifi_common.h"\n'
MENU_INCLUDE = '#include "modules/brucelink/BruceLinkModule.h" // BruceLink\n'
MENU_ENTRY_ANCHOR = '            {"Toggle BLE API",  [this]() { enableBLEAPI(); }       },\n'
MENU_ENTRY = '            {"Toggle BruceLink", [this]() { toggleBruceLink(); }   }, // BruceLink\n'

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


def copy_tree(sources, dest):
    """Replace dest with exactly the files matched in sources (flat)."""
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    names = {}
    for src_dir, patterns in sources:
        for pattern in patterns:
            for f in sorted(src_dir.glob(pattern)):
                if f.name in names:
                    fail(f"{f} and {names[f.name]} would collide in {dest}")
                names[f.name] = f
                shutil.copyfile(f, dest / f.name)
    return len(names)


def patch_menu(path, uninstall):
    raw = path.read_bytes()
    crlf = b"\r\n" in raw  # keep the checkout's line endings (Windows autocrlf)
    text = raw.decode("utf-8").replace("\r\n", "\n")
    # Always start from the unpatched text so re-running never duplicates.
    clean = text.replace(MENU_INCLUDE, "").replace(MENU_ENTRY, "")
    if uninstall:
        new = clean
    else:
        for anchor in (MENU_INCLUDE_ANCHOR, MENU_ENTRY_ANCHOR):
            if clean.count(anchor) != 1:
                fail(f"{path}: expected exactly one line\n    {anchor.strip()}\n"
                     f"  (found {clean.count(anchor)}). This Bruce version differs from "
                     f"{PINNED_BRUCE}; add the menu entry by hand:\n    {MENU_INCLUDE.strip()}\n"
                     f"    {MENU_ENTRY.strip()}")
        new = clean.replace(MENU_INCLUDE_ANCHOR, MENU_INCLUDE_ANCHOR + MENU_INCLUDE)
        new = new.replace(MENU_ENTRY_ANCHOR, MENU_ENTRY_ANCHOR + MENU_ENTRY)
    if new != text:
        out = new.replace("\n", "\r\n") if crlf else new
        path.write_bytes(out.encode("utf-8"))
    return new != text


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("bruce", type=Path, help="path to a Bruce firmware checkout")
    ap.add_argument("--force", action="store_true",
                    help=f"proceed even if the checkout is not Bruce {PINNED_BRUCE}")
    ap.add_argument("--uninstall", action="store_true", help="remove BruceLink from the checkout")
    args = ap.parse_args()

    bruce = args.bruce.resolve()
    menu = bruce / MENU_FILE
    if not (bruce / "platformio.ini").is_file() or not menu.is_file():
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
        patch_menu(menu, uninstall=True)
        print(f"BruceLink removed from {bruce}")
        return

    # Menu first: if this Bruce's menu is not what we expect, stop before
    # copying anything, so a failed install leaves the tree untouched.
    changed = patch_menu(menu, uninstall=False)
    n_lib = copy_tree(LIB_SOURCES, lib_dest)
    (lib_dest / "library.json").write_text(LIBRARY_JSON, encoding="utf-8", newline="\n")
    n_mod = copy_tree([(MODULE_SOURCE, MODULE_PATTERNS)], mod_dest)

    print(f"BruceLink installed into {bruce}")
    print(f"  lib/BruceLink/            {n_lib} files + library.json")
    print(f"  src/modules/brucelink/    {n_mod} files")
    print(f"  {MENU_FILE}  {'menu entry added' if changed else 'menu entry already present'}")
    print("Build:  pio run -e m5stack-sticks3")
    print("On the device:  Config > Advanced > Toggle BruceLink")


if __name__ == "__main__":
    main()
