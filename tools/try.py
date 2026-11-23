"""Boot RDX headless, run a few shell commands, and print what came back.
Handy for poking at one thing without the full smoke test.

Usage: python tools/try.py "c-comp sample.c" "sample 5"
Uses a scratch disk (build/try-disk.img), never your build/disk.img.
"""
import os
import sys as _sys
_sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # (pages may print any character)
import re
import shutil
import socket
import subprocess
import sys
import time

# The shell prompt is "rdx> " at the root, or "rdx/path> " in a subdirectory.
PROMPT = re.compile(r"rdx(/\S*)?> ")
# Programs in a pseudo-terminal send colours as ANSI escape sequences; the
# log keeps the text without them.
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


# Serial output arrives a few bytes at a time, so an escape sequence is
# often split across reads: an unfinished one at the end is held back until
# the rest of it comes.
pending = ""


def take(data):
    global pending
    text = pending + data.decode("utf-8", "replace")
    pending = ""
    cut = text.rfind("\x1b")
    if cut >= 0 and not ANSI.match(text, cut) and len(text) - cut < 32:
        text, pending = text[:cut], text[cut:]
    return ANSI.sub("", text)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESP = os.path.join(ROOT, "build", "esp")
DISK = os.path.join(ROOT, "build", "try-disk.img")
PORT = 45560
TIMEOUT = float(os.environ.get("TRY_TIMEOUT", "30"))

qemu = shutil.which("qemu-system-x86_64") or r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ovmf = os.path.join(os.path.dirname(qemu), "share", "edk2-x86_64-code.fd")

with open(DISK, "wb") as f:
    f.write(b"RDXDISK1")
    f.truncate(16 * 1024 * 1024)

# TRY_MACHINE picks the QEMU machine (e.g. "q35,hpet=off"), TRY_EXTRA adds
# arguments (e.g. "-device qemu-xhci -device usb-kbd"), TRY_BOOTLOG=1 also
# prints the kernel's boot messages.
proc = subprocess.Popen([
    qemu, "-machine", os.environ.get("TRY_MACHINE", "q35"), "-m", os.environ.get("TRY_MEM", "256M"),
    ] + sum([["-accel", a] for a in os.environ.get("RDX_ACCEL", "whpx,tcg").split(",")], []) + [
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", f"format=raw,file=fat:rw:{ESP}",
] + ([] if os.environ.get("TRY_NODISK") else ["-drive", f"format=raw,file={DISK}"]) + [
    "-net", "none", "-no-reboot", "-display", "none",
    "-serial", f"tcp:127.0.0.1:{PORT},server=on,wait=off",
] + os.environ.get("TRY_EXTRA", "").split())

log = ""


def wait_for(sock, text, timeout):
    global log
    start = len(log)
    deadline = time.time() + timeout
    sock.settimeout(0.2)

    def seen():
        return PROMPT.search(log[start:]) if text is None else (text in log[start:])

    while time.time() < deadline:
        if seen():
            return True
        try:
            data = sock.recv(4096)
            if data:
                log += take(data)
        except socket.timeout:
            pass
    return bool(seen())


try:
    sock = None
    for _ in range(50):
        try:
            sock = socket.create_connection(("127.0.0.1", PORT), timeout=1)
            break
        except OSError:
            time.sleep(0.1)
    if not wait_for(sock, None, 60):
        print("RDX did not reach the shell prompt")
    mark = 0 if os.environ.get("TRY_BOOTLOG") else len(log)
    for command in sys.argv[1:]:
        started = time.time()
        sock.sendall(command.encode() + b"\r")
        if not wait_for(sock, None, TIMEOUT):
            log += f"\n[try.py: no prompt within {TIMEOUT:.0f}s]\n"
            break
        log += f"[{time.time() - started:.2f}s] "
    print(log[mark:].replace("\r\n", "\n"))
finally:
    proc.kill()
