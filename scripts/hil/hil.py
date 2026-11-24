#!/usr/bin/env python3
"""BruceLink hardware-in-the-loop helper: detect, identify, back up, log, check.

Needs `pyserial`, and esptool 5 for the StickS3 backup. Verified on Windows;
on Linux and macOS it is written to the same rules but not yet run with the
devices. Nothing here writes to a device:

  detect                 list the Flipper (USB 0483:5740) and the StickS3
                         (ESP32-S3 USB-Serial/JTAG 303A:*) by VID/PID
  flipper-info           firmware identity, BruceLink files and their MD5s
                         (Flipper CLI read commands only; no reset)
  flipper-backup DIR     copy the BruceLink app and the files directly in its
                         data folder (not subfolders) off the Flipper's SD
                         card, each verified against the Flipper's own MD5
  sticks3-log            log the StickS3's serial output WITHOUT resetting it
                         (see open_no_reset)
  sticks3-backup DIR     read the whole StickS3 flash with esptool, verify it
                         against the chip (`verify-flash`, read-only), and
                         describe it (partitions, app identity). The StickS3
                         REBOOTS (each esptool step enters download mode and
                         ends in a reset); nothing is written to it
  check                  non-disruptive regression check of both devices
                         against the pins; JSON report; exit 1 on a mismatch

Logs and reports go to hil-logs/ (git-ignored) unless --out is given. A
backup holds pairing keys (the StickS3's NVS, the Flipper's key file), so DIR
must be outside any git checkout (refused otherwise); never share a backup.
Serial logs can hold secrets too:
Bruce prints its whole bruce.conf (possibly Wi-Fi/web UI credentials) at boot.
Procedures: docs/HARDWARE_TESTING.md.
"""

import argparse
import datetime as _dt
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # the device commands need it (main() says so); the
    serial = list_ports = None  # parsers below do not (tests/test_hil.py)

REPO = Path(__file__).resolve().parents[2]
FLIPPER_VID, FLIPPER_PID = 0x0483, 0x5740
ESP_VID = 0x303A
PROMPT = b"\r\n>: "

# Pins the devices are checked against (docs/HARDWARE_TESTING.md, section 1).
EXPECT_FLIPPER = {
    "firmware_version": "mntm-012",
    "firmware_api_major": "87",
    "firmware_api_minor": "1",
    "firmware_target": "7",
}
FLIPPER_APP = "/ext/apps/Bluetooth/brucelink.fap"
FLIPPER_DATA = "/ext/apps_data/brucelink"
INFO_KEYS = re.compile(
    r"^(firmware_(version|origin_fork|commit|branch|build_date|api_major|api_minor|target)"
    r"|hardware_(model|ver|target|region)|radio_stack_(type|major|minor|sub|release))$"
)


def ts():
    return _dt.datetime.now().strftime("%Y%m%dT%H%M%S")


def out_dir(arg):
    d = Path(arg) if arg else REPO / "hil-logs"
    d.mkdir(parents=True, exist_ok=True)
    return d


def in_git_checkout(path):
    """Whether `path` (existing or not) lies inside this or any git checkout."""
    d = Path(path).resolve()
    if d == REPO or REPO in d.parents:
        return True
    probe = d
    while not probe.exists() and probe != probe.parent:
        probe = probe.parent
    try:
        r = subprocess.run(["git", "-C", str(probe), "rev-parse", "--show-toplevel"],
                           capture_output=True, text=True)
    except OSError:  # no git: the check above is all there is
        return False
    return r.returncode == 0


def backup_root(arg):
    """The folder a backup goes into. Backups hold pairing keys, so never in
    a git checkout: there an ignored file is one `git add -f` from a commit,
    and a worktree's ignored files vanish when the worktree is removed."""
    if in_git_checkout(arg):
        sys.exit(f"{Path(arg).resolve()} is inside a git checkout: back up to a folder outside it")
    d = Path(arg)
    d.mkdir(parents=True, exist_ok=True)
    return d


# --- detection -------------------------------------------------------------

def find_ports():
    fl, es = [], []
    for p in list_ports.comports():
        if p.vid == FLIPPER_VID and p.pid == FLIPPER_PID:
            fl.append(p)
        elif p.vid == ESP_VID:
            es.append(p)
    return fl, es


def port_or_detect(port, kind):
    if port:
        return port
    fl, es = find_ports()
    found = fl if kind == "flipper" else es
    if not found:
        sys.exit(f"{kind}: not found (pass --port)")
    return found[0].device


def cmd_detect(_a):
    fl, es = find_ports()
    for name, ports in (("Flipper", fl), ("StickS3", es)):
        if not ports:
            print(f"{name:8} not found")
        for p in ports:
            print(f"{name:8} {p.device:8} {p.vid:04X}:{p.pid:04X} serial={p.serial_number} {p.description}")
    return 0


# --- Flipper CLI -----------------------------------------------------------

class FlipperCli:
    """Minimal Flipper CLI client over the USB CDC port (read commands only)."""

    def __init__(self, port):
        self.s = serial.Serial(port, 230400, timeout=0.2)
        self.s.dtr = True  # the Flipper starts its CLI session on DTR
        time.sleep(0.5)
        self.s.write(b"\r")
        self._until(PROMPT, 3.0)

    def close(self):
        self.s.close()

    def _until(self, marker, timeout, buf=b""):
        """Read until `marker` has arrived; `buf` holds what was already read."""
        end = time.monotonic() + timeout
        while marker not in buf:
            if time.monotonic() >= end:
                raise TimeoutError(f"no {marker!r} within {timeout}s (got {buf[-80:]!r})")
            buf += self.s.read(4096)
        return buf

    def run(self, cmd, timeout=5.0):
        self.s.reset_input_buffer()
        self.s.write(cmd.encode() + b"\r")
        raw = self._until(PROMPT, timeout)
        text = raw[: raw.rfind(PROMPT)].decode(errors="replace")
        lines = text.split("\r\n")
        return "\n".join(lines[1:]).strip()  # drop the echoed command

    def read_file(self, path, timeout=30.0):
        """Exact bytes of a file (`storage read` prints `Size: N` then raw data)."""
        self.s.reset_input_buffer()
        self.s.write(f"storage read {path}".encode() + b"\r")
        head = b""
        end = time.monotonic() + timeout
        while b"Size: " not in head or not re.search(rb"Size: \d+\r\n", head):
            chunk = self.s.read(256)
            head += chunk
            if PROMPT in head and b"Size: " not in head:
                raise FileNotFoundError(head.decode(errors="replace").strip())
            if time.monotonic() > end:
                raise TimeoutError(f"storage read {path}: no size line")
        m = re.search(rb"Size: (\d+)\r\n", head)
        size = int(m.group(1))
        data = head[m.end():]
        while len(data) < size:
            chunk = self.s.read(size - len(data))
            if not chunk and time.monotonic() > end:
                raise TimeoutError(f"storage read {path}: {len(data)}/{size} bytes")
            data += chunk
        rest = data[size:]
        data = data[:size]
        # The read that finished the file may also hold part of the prompt.
        self._until(PROMPT, 3.0, rest)
        return data

    def md5(self, path):
        out = self.run(f"storage md5 {path}")
        m = re.search(r"\b([0-9a-f]{32})\b", out)
        return m.group(1) if m else None

    def listdir(self, path):
        out = self.run(f"storage list {path}")
        entries = []
        for line in out.splitlines():
            m = re.match(r"\s*\[([FD])\]\s+(\S+)(?:\s+(\d+)b)?", line)
            if m:
                entries.append((m.group(1), m.group(2), int(m.group(3)) if m.group(3) else None))
        return entries

    def info(self):
        out = self.run("device_info", timeout=8.0)
        info = {}
        for line in out.splitlines():
            if ":" in line:
                k, v = line.split(":", 1)
                info[k.strip()] = v.strip()
        return info


def fap_code_digest(blob):
    """SHA-256 over a FAP's loadable content: every ELF section except
    `.gnu_debuglink` (it names the build's debug file, so two builds of the
    same source differ there and nowhere else)."""
    if blob[:4] != b"\x7fELF" or blob[4] != 1:
        raise ValueError("not an ELF32 file")
    shoff, = struct.unpack_from("<I", blob, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    hdr = lambda i: struct.unpack_from("<IIIIIIIIII", blob, shoff + i * shentsize)
    str_off = hdr(shstrndx)[4]
    h = hashlib.sha256()
    for i in range(shnum):
        name_i, sh_type, _, _, off, size = hdr(i)[:6]
        name = blob[str_off + name_i : blob.index(b"\0", str_off + name_i)].decode()
        if sh_type == 8 or name == ".gnu_debuglink":  # SHT_NOBITS has no bytes
            continue
        h.update(name.encode() + b"\0" + blob[off : off + size])
    return h.hexdigest()


def flipper_identity(cli, show_all=False):
    info = cli.info()
    ident = info if show_all else {k: v for k, v in info.items() if INFO_KEYS.match(k)}
    files = {}
    for kind, name, size in cli.listdir(FLIPPER_DATA):
        if kind == "F":
            path = f"{FLIPPER_DATA}/{name}"
            files[path] = {"size": size}  # data files: size only (key material)
    app = [e for e in cli.listdir(FLIPPER_APP.rsplit("/", 1)[0]) if e[1] == "brucelink.fap"]
    if app:
        files[FLIPPER_APP] = {"size": app[0][2], "md5": cli.md5(FLIPPER_APP)}
    return ident, files


def cmd_flipper_info(a):
    cli = FlipperCli(port_or_detect(a.port, "flipper"))
    try:
        ident, files = flipper_identity(cli, a.all)
    finally:
        cli.close()
    for k, v in ident.items():
        print(f"  {k:28}: {v}")
    for path, meta in files.items():
        print(f"  {path}: {meta}")
    return 0


def cmd_flipper_backup(a):
    dest = backup_root(a.dir) / f"flipper-{ts()}"
    cli = FlipperCli(port_or_detect(a.port, "flipper"))
    manifest = {"time": ts(), "files": {}}
    try:
        manifest["identity"] = flipper_identity(cli)[0]
        paths = [FLIPPER_APP] + [
            f"{FLIPPER_DATA}/{n}" for k, n, _ in cli.listdir(FLIPPER_DATA) if k == "F"
        ]
        for path in paths:
            data = cli.read_file(path)
            want = cli.md5(path)
            got = hashlib.md5(data).hexdigest()
            local = dest / path.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(data)
            ok = want == got
            manifest["files"][path] = {
                "size": len(data), "md5": got, "md5_on_flipper": want, "verified": ok,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
            print(f"  {'OK ' if ok else 'BAD'} {path} ({len(data)} B)")
    finally:
        cli.close()
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"backup: {dest}")
    return 0 if all(f["verified"] for f in manifest["files"].values()) else 1


# --- StickS3 ---------------------------------------------------------------

def open_no_reset(port, baud=115200, posix=None):
    """Open the ESP32-S3 port without resetting the chip.

    The USB-Serial/JTAG holds the chip in reset while RTS is on and DTR off.
    On Windows pyserial applies both lines at once, so DTR and RTS off from
    the start is safe (verified on the StickS3). On Linux and macOS the kernel
    turns both lines on at open and pyserial then sets DTR before RTS, so the
    same settings would pass through DTR off + RTS on. There, start from DTR
    on + RTS off and drop DTR once open: on/on, on/off, off/off. That order
    is not yet verified on hardware.
    """
    posix = os.name == "posix" if posix is None else posix
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.2
    s.dtr = posix
    s.rts = False
    s.open()
    if posix:
        s.dtr = False
    return s


def capture_log(port, seconds, path):
    s = open_no_reset(port)
    lines = []
    try:
        end = time.monotonic() + seconds
        buf = b""
        with open(path, "a", encoding="utf-8") as f:
            while time.monotonic() < end:
                buf += s.read(1024)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    txt = line.decode(errors="replace").rstrip("\r")
                    stamp = _dt.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                    f.write(f"[{stamp}] {txt}\n")
                    f.flush()
                    lines.append(txt)
                    if not quiet_log:
                        print(f"[{stamp}] {txt}")
    finally:
        s.close()
    return lines


quiet_log = False


def cmd_sticks3_log(a):
    port = port_or_detect(a.port, "sticks3")
    path = Path(a.out) if a.out else out_dir(None) / f"sticks3-serial-{ts()}.log"
    print(f"logging {port} (no reset) -> {path} for {a.seconds}s")
    lines = capture_log(port, a.seconds, path)
    print(f"{len(lines)} lines")
    return 0


# Bruce serial CLI commands that only report (src/core/serial_commands/
# util_commands.cpp). Anything else is refused here: this tool never drives
# Bruce's menus or tools.
STICKS3_READONLY = ("info", "free", "uptime")


def sticks3_cli(port, command, seconds=1.5):
    if command not in STICKS3_READONLY:
        raise ValueError(f"only read-only commands: {', '.join(STICKS3_READONLY)}")
    s = open_no_reset(port)
    try:
        s.reset_input_buffer()
        s.write(command.encode() + b"\n")
        buf, end = b"", time.monotonic() + seconds
        while time.monotonic() < end:
            buf += s.read(1024)
    finally:
        s.close()
    return buf.decode(errors="replace").replace("\r", "").strip()


def cmd_sticks3_cli(a):
    print(sticks3_cli(port_or_detect(a.port, "sticks3"), a.command))
    return 0


def esptool(*args):
    cmd = [sys.executable, "-m", "esptool", *args]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def verify_flash_ok(rc, text):
    """Whether `esptool verify-flash` matched. esptool 4 prints "-- verify OK
    (digest matched)", esptool 5 "Verification successful (digest matched)";
    a difference prints a mismatch and exits non-zero."""
    low = text.lower()  # (esptool echoes the file path, so match whole phrases)
    return rc == 0 and "digest matched" in low and "digest mismatch" not in low


def boot_banner(lines):
    """ESP32-S3 ROM boot lines in a serial log: the chip was reset."""
    return [l for l in lines if l.startswith("ESP-ROM:") or l.startswith("rst:0x")]


def parse_partitions(blob):
    parts = []
    for i in range(0, 0xC00, 32):
        e = blob[i : i + 32]
        if e[:2] != b"\xaa\x50":
            break
        ptype, sub, off, size = struct.unpack_from("<BBII", e, 2)
        name = e[12:28].split(b"\0", 1)[0].decode(errors="replace")
        parts.append({"name": name, "type": ptype, "subtype": sub, "offset": off, "size": size})
    return parts


def app_desc(img):
    """esp_app_desc_t from an app image (after the image + first segment header)."""
    off = 24 + 8
    if len(img) < off + 256 or struct.unpack_from("<I", img, off)[0] != 0xABCD5432:
        return None
    s = lambda a, n: img[off + a : off + a + n].split(b"\0", 1)[0].decode(errors="replace")
    return {
        "version": s(16, 32), "project_name": s(48, 32), "time": s(80, 16),
        "date": s(96, 16), "idf_ver": s(112, 32),
        "elf_sha256": img[off + 144 : off + 176].hex(),
    }


def cmd_sticks3_backup(a):
    port = port_or_detect(a.port, "sticks3")
    dest = backup_root(a.dir) / f"sticks3-{ts()}"
    dest.mkdir(parents=True, exist_ok=True)
    print("NOTE: the StickS3 reboots (download mode, then hard reset). Nothing is written.")
    rc, out = esptool("--chip", "esp32s3", "-p", port, "flash-id")
    (dest / "flash-id.txt").write_text(out)
    if rc != 0:
        print(out)
        return 1
    m = re.search(r"Detected flash size: (\d+)MB", out)
    size = int(m.group(1)) * 1024 * 1024 if m else 8 * 1024 * 1024
    image = dest / "sticks3-full.bin"
    rc, out = esptool("--chip", "esp32s3", "-p", port, "-b", str(a.baud),
                      "read-flash", "0x0", hex(size), str(image))
    (dest / "read-flash.txt").write_text(out)
    if rc != 0:
        print(out)
        return 1
    # Read-only comparison of the file against the chip (MD5 per region).
    rc, vout = esptool("--chip", "esp32s3", "-p", port, "verify-flash", "0x0", str(image))
    (dest / "verify-flash.txt").write_text(vout)
    verified = verify_flash_ok(rc, vout)
    blob = image.read_bytes()
    parts = parse_partitions(blob[0x8000:0x9000])
    app = next((p for p in parts if p["type"] == 0), None)
    desc = app_desc(blob[app["offset"] : app["offset"] + app["size"]]) if app else None
    info = {
        "time": ts(), "port": port, "flash_size": size,
        "chip": re.findall(r"(Chip (?:type|is)[^\n]*|MAC:[^\n]*|Features:[^\n]*|Crystal[^\n]*)", out + vout),
        "sha256": hashlib.sha256(blob).hexdigest(),
        "verify_flash": "verified" if verified else "FAILED",
        "bootloader_magic_ok": blob[0] == 0xE9,
        "partitions": parts, "app": desc,
    }
    (dest / "info.json").write_text(json.dumps(info, indent=2))
    (dest / "SHA256SUMS").write_text(f"{info['sha256']}  sticks3-full.bin\n")
    print(json.dumps({k: info[k] for k in ("flash_size", "sha256", "verify_flash", "app")}, indent=2))
    for p in parts:
        print(f"  {p['name']:10} type {p['type']} sub {p['subtype']:3} @0x{p['offset']:06x} 0x{p['size']:06x}")
    print(f"backup: {dest}")
    return 0 if verified else 1


# --- check -----------------------------------------------------------------

def cmd_check(a):
    global quiet_log
    report = {"time": ts(), "results": [], "ok": True}

    def res(name, ok, detail):
        report["results"].append({"check": name, "ok": ok, "detail": detail})
        report["ok"] &= bool(ok)
        print(f"  {'PASS' if ok else 'FAIL'}  {name}: {detail}")

    fl, es = find_ports()
    res("flipper.present", bool(fl), fl[0].device if fl else "not found")
    res("sticks3.present", bool(es), es[0].device if es else "not found")
    if fl:
        cli = FlipperCli(a.flipper_port or fl[0].device)
        try:
            ident, files = flipper_identity(cli)
            installed = cli.read_file(FLIPPER_APP) if (a.fap_ref and FLIPPER_APP in files) else None
        finally:
            cli.close()
        report["flipper"] = {"identity": ident, "files": files}
        for k, v in EXPECT_FLIPPER.items():
            res(f"flipper.{k}", ident.get(k) == v, f"{ident.get(k)} (expect {v})")
        app = files.get(FLIPPER_APP)
        res("flipper.app_installed", app is not None, app or "missing")
        if installed is not None:
            ref = Path(a.fap_ref).read_bytes()
            same = fap_code_digest(installed) == fap_code_digest(ref)
            exact = hashlib.md5(installed).hexdigest() == hashlib.md5(ref).hexdigest()
            res("flipper.app_matches_reference", same,
                f"{'identical' if exact else 'same code (debug link differs)' if same else 'DIFFERENT code'}"
                f" vs {a.fap_ref}")
        res("flipper.paired", f"{FLIPPER_DATA}/pairing.key" in files,
            "pairing.key present" if f"{FLIPPER_DATA}/pairing.key" in files else "no pairing.key")
    if es and a.sticks3_cli:
        sp = a.sticks3_port or es[0].device
        info = sticks3_cli(sp, "info")
        free = sticks3_cli(sp, "free")
        report["sticks3"] = {"info": info, "free": free}
        m = re.search(r"Bruce v(\S+)", info)
        res("sticks3.bruce_cli", m is not None, f"Bruce v{m.group(1)}" if m else "no reply to `info`")
    if es and a.log_seconds:
        quiet_log = True
        path = out_dir(a.out) / f"sticks3-serial-{report['time']}.log"
        lines = capture_log(a.sticks3_port or es[0].device, a.log_seconds, path)
        report["sticks3_log"] = {"file": str(path), "lines": len(lines), "tail": lines[-20:]}
        boot = boot_banner(lines)
        res("sticks3.serial_no_reset", not boot,
            f"{len(lines)} lines in {a.log_seconds}s, "
            + (f"RESET: {boot[0]}" if boot else "no boot banner"))
    path = out_dir(a.out) / f"hil-check-{report['time']}.json"
    path.write_text(json.dumps(report, indent=2))
    print(f"report: {path}  overall: {'PASS' if report['ok'] else 'FAIL'}")
    return 0 if report["ok"] else 1


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("detect").set_defaults(fn=cmd_detect)
    s = sub.add_parser("flipper-info"); s.add_argument("--port"); s.add_argument("--all", action="store_true")
    s.set_defaults(fn=cmd_flipper_info)
    s = sub.add_parser("flipper-backup"); s.add_argument("dir", help="a folder outside any git checkout")
    s.add_argument("--port")
    s.set_defaults(fn=cmd_flipper_backup)
    s = sub.add_parser("sticks3-log"); s.add_argument("--port"); s.add_argument("--seconds", type=float, default=30)
    s.add_argument("--out"); s.set_defaults(fn=cmd_sticks3_log)
    s = sub.add_parser("sticks3-backup"); s.add_argument("dir", help="a folder outside any git checkout")
    s.add_argument("--port")
    s.add_argument("--baud", type=int, default=921600); s.set_defaults(fn=cmd_sticks3_backup)
    s = sub.add_parser("sticks3-cli", help="read-only Bruce CLI command (info, free, uptime)")
    s.add_argument("command", choices=STICKS3_READONLY); s.add_argument("--port")
    s.set_defaults(fn=cmd_sticks3_cli)
    s = sub.add_parser("check")
    s.add_argument("--flipper-port"); s.add_argument("--sticks3-port")
    s.add_argument("--fap-ref", help="reference .fap to compare the installed app against "
                   "(code digest: every ELF section except .gnu_debuglink)")
    s.add_argument("--log-seconds", type=float, default=0, help="also log the StickS3 (no reset) this long")
    s.add_argument("--sticks3-cli", action="store_true",
                   help="also ask Bruce's CLI for `info` and `free` (read-only, no reset)")
    s.add_argument("--out"); s.set_defaults(fn=cmd_check)
    a = p.parse_args(argv)
    if serial is None:
        sys.exit("pyserial is required: python -m pip install pyserial")
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
