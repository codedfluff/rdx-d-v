"""Boot RDX headless in QEMU, drive the shell over the serial port, and check
that it behaves. Boots twice to prove files survive a reboot. Uses its own
scratch disk (build/test-disk.img), never your build/disk.img.

Usage: python tools/smoke.py     (after build.cmd)
Exit code 0 = all checks passed.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EFI = os.path.join(ROOT, "target", "x86_64-unknown-uefi", "release", "rdx.efi")
ESP = os.path.join(ROOT, "build", "esp")
DISK = os.path.join(ROOT, "build", "test-disk.img")
SHOT = os.path.join(ROOT, "build", "screenshot.ppm")
SERIAL_PORT, MONITOR_PORT = 45550, 45551

qemu = shutil.which("qemu-system-x86_64") or r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ovmf = os.path.join(os.path.dirname(qemu), "share", "edk2-x86_64-code.fd")

os.makedirs(os.path.join(ESP, "EFI", "BOOT"), exist_ok=True)
shutil.copyfile(EFI, os.path.join(ESP, "EFI", "BOOT", "BOOTX64.EFI"))

# Boot from a copy of the boot disk with a few extra C sources on it, so the
# in-OS compiler can be tried on the same programs clang builds.
TEST_ESP = os.path.join(ROOT, "build", "test-esp")
shutil.rmtree(TEST_ESP, ignore_errors=True)
shutil.copytree(ESP, TEST_ESP)
for name in ["hello", "primes", "wc", "grep"]:
    shutil.copyfile(os.path.join(ROOT, "user", "programs", name + ".c"),
                    os.path.join(TEST_ESP, "bin", "t_" + name + ".c"))
CSS_TEST = """<html><head><title>CSS test</title><style>
/* a comment {with braces} */
.gone { display: none }
#main p.note { font-weight: bold }
@media print { .screen { display: none } }
@media (max-width: 400px) { .wide { display: none } }
.centre { text-align: center }
ul.plain { list-style: none }
.later { display: none } .later { display: block }
</style></head><body>
<p class="gone">HIDDEN-BY-CLASS</p>
<div id="main"><p class="note">visible note</p></div>
<p style="display:none">HIDDEN-BY-STYLE</p>
<p class="screen wide">shown on screens</p>
<p class="later">the later rule wins</p>
<div style="max-width: 200px; margin: 0 auto">a narrow centred column of words</div>
<p class=centre>middle</p>
<ul class="plain"><li>no bullet here</li></ul>
<ul><li>with a bullet</li></ul>
</body></html>
"""
for name in os.listdir(os.path.join(ROOT, "tools", "webtests")):  # pages for the browser's layout and scripts
    shutil.copyfile(os.path.join(ROOT, "tools", "webtests", name), os.path.join(TEST_ESP, "bin", name))
with open(os.path.join(TEST_ESP, "bin", "csstest.html"), "w", newline="\n") as f:
    f.write(CSS_TEST)
# Test certificates (tools/gencerts.py): authorities RDX is told to trust,
# certificates from them for the test servers, signatures to check.
CERTS = os.path.join(ROOT, "build", "test-certs")
HAVE_CERTS = False
if shutil.which("openssl"):
    shutil.rmtree(CERTS, ignore_errors=True)
    HAVE_CERTS = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "gencerts.py"), CERTS],
                                capture_output=True).returncode == 0
if HAVE_CERTS:
    for name in os.listdir(CERTS):
        if name.endswith(".pem") or name.endswith(".bin"):
            shutil.copyfile(os.path.join(CERTS, name), os.path.join(TEST_ESP, "bin", name))
    with open(os.path.join(TEST_ESP, "bin", "roots.pem"), "a", newline="\n") as f:
        f.write(open(os.path.join(CERTS, "test_roots.pem")).read())
ESP = TEST_ESP

# A fresh, empty RDX data disk: signature in sector 0, zeros after.
with open(DISK, "wb") as f:
    f.write(b"RDXDISK1")
    f.truncate(16 * 1024 * 1024)

# ---------------------------------------------------------------- host-side servers
# Things for RDX to connect to. From inside QEMU this PC is 10.0.2.2.
import http.server
import socketserver
import threading
import urllib.request

BIG_TEXT = "".join(f"line {n}\n" for n in range(20000)).encode()  # about 190 KiB
PAGE = (b"<html><head><title>Host page</title><style>p{color:red}</style></head><body>"
        b"<h2>Served by the host</h2><p>Fish &amp; chips &mdash; caf\xc3\xa9 <b>bold</b></p>"
        b"<script>document.write('written by a ' + 'script')</script>"
        b"<ul><li><a href='other.html'>A relative link</a></li><li><a href='/x/../top.html'>Up and back</a></li></ul>"
        b"</body></html>")


class WebHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/old":
            self.send_response(302)
            self.send_header("Location", "/dir/page.html")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        body = {"/small.txt": b"hello from the host\n", "/big.txt": BIG_TEXT, "/dir/page.html": PAGE}.get(self.path)
        self.send_response(200 if body is not None else 404)
        body = body if body is not None else b"not here\n"
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class EchoHandler(socketserver.BaseRequestHandler):
    """Reads until the client finishes sending, then answers and closes."""

    def handle(self):
        data = b""
        while chunk := self.request.recv(4096):
            data += chunk
        self.request.sendall(b"you said: " + data)


class UdpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(b"echo: " + data, self.client_address)


def serve(server):
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server.server_address[1]


WEB_PORT = serve(http.server.ThreadingHTTPServer(("127.0.0.1", 0), WebHandler))

# The same pages over https: with an RSA certificate from the test
# authority, with an EC one through an intermediate, and with a self-signed
# one that RDX must refuse.
import ssl
HTTPS_PORT = HTTPS_EC_PORT = HTTPS_BAD_PORT = HTTPS_12_PORT = HTTPS_12_EC_PORT = None


def https_server(chain, key, tls12=False):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), WebHandler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    if tls12:
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_2
    else:
        context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.load_cert_chain(os.path.join(CERTS, chain), os.path.join(CERTS, key))
    server.socket = context.wrap_socket(server.socket, server_side=True)
    return serve(server)


if HAVE_CERTS:
    HTTPS_PORT = https_server("chain_rsa.pem", "leaf_rsa.key")
    HTTPS_EC_PORT = https_server("chain_ec.pem", "leaf_ec.key")
    HTTPS_BAD_PORT = https_server("self.pem", "self.key")
    HTTPS_12_PORT = https_server("chain_rsa.pem", "leaf_rsa.key", tls12=True)
    HTTPS_12_EC_PORT = https_server("chain_ec.pem", "leaf_ec.key", tls12=True)
ECHO_PORT = serve(socketserver.ThreadingTCPServer(("127.0.0.1", 0), EchoHandler))
UDP_PORT = serve(socketserver.ThreadingUDPServer(("127.0.0.1", 0), UdpHandler))
with socket.socket() as probe:  # a free port to forward to RDX's own web server
    probe.bind(("127.0.0.1", 0))
    FORWARD_PORT = probe.getsockname()[1]

log = ""
failures = []
serial = None
mon = None
proc = None


def connect(port):
    for _ in range(50):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=1)
        except OSError:
            time.sleep(0.1)
    raise RuntimeError(f"could not connect to QEMU on port {port}")


def start_qemu(machine="q35", extra=(), sata_disk=True):
    global serial, mon, proc, cursor
    cursor = len(log)
    proc = subprocess.Popen([
        qemu, "-machine", machine, "-m", "256M",
        "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
        "-drive", f"format=raw,file=fat:rw:{ESP}",
    ] + (["-drive", f"format=raw,file={DISK}"] if sata_disk else []) + [
        "-net", "none", "-no-reboot", "-display", "none",
        "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=off",
        "-monitor", f"tcp:127.0.0.1:{MONITOR_PORT},server=on,wait=off",
    ] + list(extra))
    serial = connect(SERIAL_PORT)
    mon = connect(MONITOR_PORT)


def stop_qemu():
    try:
        mon.sendall(b"quit\n")
        proc.wait(timeout=10)
    except Exception:  # noqa: BLE001
        proc.kill()
    serial.close()
    mon.close()
    time.sleep(0.5)


# Programs in a pseudo-terminal send colours and cursor movement as ANSI
# escape sequences; the log keeps the text without them.
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


# Output is consumed in order: `cursor` is how far into the log the checks
# have read. Each wait_for looks for its text after the cursor and moves the
# cursor past it. (Looking only at output that arrives after the call would
# miss text that came in the same burst as the thing waited for before it,
# which is the normal case now that RDX answers within microseconds.)
cursor = 0


def wait_for(text, timeout):
    """Read serial output until `text` shows up after the cursor."""
    global log, cursor
    deadline = time.time() + timeout
    serial.settimeout(0.2)
    while True:
        found = log.find(text, cursor)
        if found >= 0:
            cursor = found + len(text)
            return True
        if time.time() >= deadline:
            return False
        try:
            data = serial.recv(4096)
            if data:
                log += take(data)
        except (socket.timeout, OSError):
            pass


def send(data):
    """Type something. Output that is already on its way is taken in first
    and skipped, so that later waits only see what this input causes."""
    global log, cursor
    serial.settimeout(0.03)
    while True:
        try:
            chunk = serial.recv(4096)
        except (socket.timeout, OSError):
            break
        if not chunk:
            break
        log += take(chunk)
    cursor = len(log)
    serial.sendall(data)


def run(cmd, expect, timeout=10):
    """Type a command and wait for `expect` in what comes back."""
    send(cmd.encode() + b"\r")
    return wait_for(expect, timeout)


def check(name, passed):
    print(f"{'PASS' if passed else 'FAIL'}  {name}")
    if not passed:
        failures.append(name)


def used_memory():
    run("mem", "identity-mapped")
    wait_for("rdx> ", 5)
    return re.findall(r"physical: ([\d.]+) MiB used", log)[-1]


try:
    # ================================================================ first boot
    start_qemu()
    check("boots to shell prompt", wait_for("rdx> ", 60))
    check("SATA disk found and mounted", "saved file(s) restored from SATA disk" in log)

    check("help lists commands", run("help", "shutdown"))
    check("the shell is a ring 3 program started by init",
          run("ps", "sh.exe") and wait_for("rdx> ", 5)
          and re.search(r" 3 +[\d.]+ +sh\.exe", log[log.rindex(" PID  TID"):]) is not None
          and re.search(r" 0 +[\d.]+ +init", log[log.rindex(" PID  TID"):]) is not None)
    check("/proc files describe the kernel",
          run("cat /proc/version", "RDX 0.2.0\r\n") and wait_for("rdx> ", 5)
          and run("ls /proc", "memmap") and wait_for("rdx> ", 5)
          and run("cat /proc/uptime | wc -l", "1\r\n") and wait_for("rdx> ", 5))
    check("/proc cannot be written to",
          run("write /proc/version hacked", "cannot be changed") and wait_for("rdx> ", 5)
          and run("rm /proc/mem", "rdx> ") and run("cat /proc/version", "RDX 0.2.0\r\n") and wait_for("rdx> ", 5))
    check("exit leaves the shell and init starts a new one",
          run("set GONE=yes", "rdx> ") and run("exit", "Type 'help'", 10) and wait_for("rdx> ", 5)
          and run("echo [$GONE]", "[]\r\n") and wait_for("rdx> ", 5))
    check("sh runs a file of commands, and reads commands from a pipe",
          run("echo echo from-sh-file > s.rc", "rdx> ") and run("sh s.rc", "from-sh-file\r\n") and wait_for("rdx> ", 5)
          and run("echo echo piped-into-sh | sh", "piped-into-sh\r\n") and wait_for("rdx> ", 5))
    check("mem reports heap", run("mem", "identity-mapped"))
    check("cpu identifies processor", run("cpu", "flags:"))
    check("write + cat", run("write t.txt hello from rdx", "rdx> ") and run("cat t.txt", "hello from rdx"))
    check("timer ticks (sleep returns)", run("sleep 300", "rdx> ") and run("uptime", "ticks)"))
    check("preemptive tasks run to completion", run("spawn 2", "tick 5 of 5", 15))
    check("faulting task is killed, kernel survives",
          run("crash", "crashed in kernel mode: page fault") and run("echo still-alive", "still-alive\r\n"))
    check("ps works after reaping", run("ps", "sh.exe"))

    # ---- user mode: C programs loaded from \bin on the boot disk ----
    check("programs loaded from boot disk", run("ls", "hello.exe"))
    before = used_memory()
    check("hello.exe runs in ring 3 with arguments",
          run("hello one two", "argument 2: two") and wait_for("rdx> ", 5))
    check("primes.exe (heap + preemption) gets the right answer",
          run("primes 1000000", "78498 primes below 1000000", 30) and wait_for("rdx> ", 5))
    check("evil.exe cannot touch kernel memory",
          run("evil", "crashed in user mode: page fault") and "protection is broken" not in log
          and run("echo kernel-ok", "kernel-ok\r\n"))

    # Play guess.exe to the end with a binary search (keyboard input syscall).
    won = run("guess", "Guess 1: ")
    lo, hi = 1, 100
    for _ in range(8):
        if not won:
            break
        mid = (lo + hi) // 2
        mark = len(log)
        send(f"{mid}\r".encode())
        deadline = time.time() + 5
        while time.time() < deadline and not re.search("Too low|Too high|Correct", log[mark:]):
            wait_for("\n", 0.3)
        reply = log[mark:]
        if "Correct" in reply:
            break
        if "Too low" in reply:
            lo = mid + 1
        elif "Too high" in reply:
            hi = mid - 1
        else:
            won = False
    check("guess.exe is winnable (interactive input)", won and "Correct!" in log and wait_for("rdx> ", 5))
    check("guess.exe saved its score to a file", run("cat guess.best", "rdx> ") and run("guess", "Best so far"))

    send(b"\x03")
    check("Ctrl+C terminates the foreground program", wait_for("was terminated", 5))
    check("Windows-style junk is rejected cleanly",
          run("write fake.exe hello", "rdx> ") and run("fake", "not an executable"))

    # ---- editor: type two lines, go back up and insert at the start, save, quit ----
    send(b"edit note.txt\r")
    time.sleep(1.0)
    for chunk in [b"hello\rworld", b"\x80", b"\x84", b"X", b"\x13", b"\x11"]:  # up, home, X, ^S, ^Q
        send(chunk)
        time.sleep(0.4)
    check("edit.exe returns to the shell", wait_for("rdx> ", 10))
    check("edit.exe saved what was typed", run("cat note.txt", "Xhello\r\nworld\r\n"))

    # ---- RX scripting language ----
    check("rx runs demo.rx", run("rx demo", "Reversed:  XR snur XDR", 30)
          and "0 1 1 2 3 5 8 13 21 34 55 89" in log and "boot, heap, kernel, page, ring, task" in log
          and wait_for("checksum 599994", 60) and wait_for("rdx> ", 10))
    check("rx runs a script written inside RDX",
          run("write t.rx print(6 * 7, [1, \"two\"])", "rdx> ") and run("rx t.rx", "42 [1, \"two\"]\r\n"))
    check("rx reports runtime errors with a line number",
          run("write bad.rx print(1 / 0)", "rdx> ") and run("rx bad.rx", "bad.rx:1: division by zero"))
    check("rx reports syntax errors",
          run("write bad2.rx let x = = 2", "rdx> ") and run("rx bad2.rx", "bad2.rx:1: expected a value"))
    wait_for("rdx> ", 5)
    send(b"rx\r")
    repl_ok = wait_for("rx> ", 10)
    send(b"fn sq(n) { return n * n }\r")
    repl_ok = repl_ok and wait_for("rx> ", 5)
    send(b"sq(12) + 1\r")
    repl_ok = repl_ok and wait_for("145\r\n", 5)
    send(b"exit()\r")
    check("rx interactive prompt", repl_ok and wait_for("rdx> ", 5))
    send(b"rx worm\r")
    time.sleep(1.5)
    send(b"\x81")  # steer down
    time.sleep(0.5)
    send(b"q")
    check("rx runs worm.rx (full-screen game) until quit", wait_for("Game over. Final score:", 10)
          and wait_for("rdx> ", 5))

    # ---- pipes, redirection, shared memory, core utilities ----
    check("pipeline of two programs", run("seq 5 | wc -l", "5\r\n") and wait_for("rdx> ", 5))
    check("pipeline of four programs",
          run("ls | grep exe | sort -r | head -3", "yes.exe\r\nwrite.exe\r\nwget.exe\r\n") and wait_for("rdx> ", 5))
    check("output redirection, then reading it back through a pipe",
          run("echo one two three > words.txt", "rdx> ") and run("cat words.txt | rev", "eerht owt eno\r\n")
          and wait_for("rdx> ", 5))
    check("input redirection and append",
          run("echo more >> words.txt", "rdx> ") and run("wc -l < words.txt", "2\r\n") and wait_for("rdx> ", 5))
    check("an endless writer is stopped when the reader finishes",
          run("yes hi | head -2", "hi\r\nhi\r\n") and wait_for("rdx> ", 5)
          and run("ps", "sh.exe") and "yes.exe" not in log[log.rindex(" PID  TID"):])
    check("numeric sort, tail, tee, uniq",
          run("seq 20 | sort -rn | tail -2", "2\r\n1\r\n") and wait_for("rdx> ", 5)
          and run("seq 3 | tee copy.txt | wc -l", "3\r\n") and wait_for("rdx> ", 5)
          and run("cat copy.txt copy.txt | sort | uniq -c | head -1", "   2 1\r\n") and wait_for("rdx> ", 5))
    check("grep options and hexdump",
          run("grep -c e words.txt", "2\r\n") and wait_for("rdx> ", 5)
          and run("grep -vn one words.txt", "2:more\r\n") and wait_for("rdx> ", 5)
          and run("echo AB | hexdump", "41 42 0a") and wait_for("rdx> ", 5))
    check("shared memory + pipe + spawn + wait between two processes",
          run("shmdemo", "shared sum = 396 (expected 396), note = \"written by the child process\"")
          and "(exit code 0)" in log and wait_for("rdx> ", 5))
    check("unknown program in a pipeline is reported", run("nosuch | wc", "unknown command or program: nosuch"))

    # ---- directories: mkdir, cd, pwd, nested paths, rm -r ----
    check("mkdir and cd change the prompt",
          run("mkdir project", "rdx> ") and run("cd project", "rdx/project> "))
    check("pwd reports the path", run("pwd", "/project\r\n") and wait_for("rdx/project> ", 5))
    check("files live inside the directory",
          run("write hello.txt inside a folder", "rdx/project> ")
          and run("cat hello.txt", "inside a folder\r\n") and wait_for("rdx/project> ", 5))
    check("nested directories and relative paths",
          run("mkdir src", "rdx/project> ") and run("write src/main.c int main(void){}", "rdx/project> ")
          and run("ls", "src/") and run("cat src/main.c", "int main(void){}\r\n")
          and wait_for("rdx/project> ", 5))
    check("cd .. goes up, absolute paths work",
          run("cd ..", "rdx> ") and run("cat project/src/main.c", "int main(void){}\r\n")
          and run("ls project", "hello.txt\r\nsrc/\r\n") and wait_for("rdx> ", 5))
    check("a program runs from a subdirectory and sees that directory",
          run("cd project", "rdx/project> ") and run("seq 3 > nums.txt", "rdx/project> ")
          and run("wc -l nums.txt", "3\r\n") and run("ls", "nums.txt")
          and wait_for("rdx/project> ", 5) and run("cd /", "rdx> "))
    check("find searches subdirectories",
          run("find folder", "project/hello.txt\r\n", 15) and wait_for("rdx> ", 5))
    check("tree prints the directory hierarchy",
          run("tree project", "1 directory", 10) and "|-- hello.txt" in log and "`-- src/" in log
          and "main.c" in log and wait_for("rdx> ", 5))
    check("ls of a path, and a trailing-slash directory marker",
          run("ls project", "src/") and wait_for("rdx> ", 5))
    check("rmdir refuses a non-empty directory",
          run("rmdir project", "not empty") and wait_for("rdx> ", 5))
    check("rm -r removes a directory tree",
          run("rm -r project", "rdx> ") and run("cat project/src/main.c", "no such file")
          and wait_for("rdx> ", 5))
    # ---- shell line editing: tab completion and command history ----
    send(b"prim\t")  # unique program name -> "primes "
    time.sleep(0.4)
    send(b"50\r")
    check("tab completes a command name", wait_for("primes below 50", 10) and wait_for("rdx> ", 5))
    send(b"cat readm\t")  # unique file name -> "readme.txt "
    time.sleep(0.4)
    send(b"\r")
    check("tab completes a file name", wait_for("Welcome to RDX", 10) and wait_for("rdx> ", 5))
    run("echo history_probe", "history_probe\r\n")
    wait_for("rdx> ", 5)
    run("pwd", "/\r\n")  # a different command in between
    wait_for("rdx> ", 5)
    mark = len(log)
    send(b"\x80\x80")  # Up twice: skip past pwd, back to the echo
    time.sleep(0.4)
    send(b"\r")
    check("up-arrow recalls an earlier command",
          wait_for("history_probe\r\n", 10) and "history_probe" in log[mark:] and wait_for("rdx> ", 5))

    check("set up a directory to survive the reboot",
          run("mkdir saved_dir", "rdx> ") and run("write saved_dir/kept.txt directories persist", "rdx> "))

    # Left/Right, Home/End and Delete edit in the middle of the line.
    send(b"echo acd\x82\x82b\x85e\r")  # Left Left, insert b, End, append e
    check("left/right arrows insert in the middle of a line", wait_for("abcde\r\n", 5) and wait_for("rdx> ", 5))
    send(b"Xecho home-del\x84\x88\r")  # Home, Delete removes the X
    check("Home and Delete edit the start of a line", wait_for("home-del\r\n", 5) and wait_for("rdx> ", 5))
    send(b"echo wrong\x15echo right-line\r")  # Ctrl+U wipes the line
    mark = len(log)
    check("Ctrl+U clears the line", wait_for("right-line\r\n", 5) and "wrong\r\n" not in log[mark:]
          and wait_for("rdx> ", 5))

    # ---- blocking: a waiting program is woken the moment its data arrives ----
    run("pingpong 500", "round trips in", 30)
    wait_for("rdx> ", 10)
    each = re.findall(r"\((\d+) microseconds each\)", log)
    check(f"pipe round trip wakes the other side at once ({each[-1] if each else '?'} us; a tick is 10000)",
          bool(each) and int(each[-1]) < 3000)
    # A snapshot can catch the shell in the instant it is woken to look at a
    # child that just finished, so allow a few tries.
    def shell_seen_waiting():
        run("ps | grep sh.exe", "sh.exe")
        wait_for("rdx> ", 5)
        return re.search(r"waiting +- +3 +[\d.]+ +sh\.exe", log[log.rindex("ps | grep sh.exe"):]) is not None

    check("ps shows the shell (a ring 3 program) waiting, not polling",
          any(shell_seen_waiting() for _ in range(3)))

    # ---- signals: programs can catch Ctrl+C and kill ----
    mark = len(log)
    send(b"sigtest\r")
    caught = wait_for("ready", 10)
    for n in (1, 2, 3):
        send(b"\x03")
        caught = caught and wait_for(f"caught interrupt {n} of 3", 5)
    check("a program catches Ctrl+C and exits in its own time",
          caught and wait_for("sigtest finished cleanly", 5) and wait_for("rdx> ", 5)
          and "was terminated" not in log[mark:])
    mark = len(log)
    send(b"sigtest wait\r")
    caught = wait_for("ready", 10)
    send(b"\x03")
    caught = caught and wait_for("caught interrupt 1 of 3", 5)
    send(b"still here\r")  # the interrupted read() carries on
    caught = caught and wait_for("read: still here", 5)
    send(b"\x03")
    caught = caught and wait_for("caught interrupt 2 of 3", 5)
    send(b"\x03")
    check("a signal interrupts a program waiting for input, which then resumes",
          caught and wait_for("sigtest finished cleanly", 5) and wait_for("rdx> ", 5)
          and "was terminated" not in log[mark:])

    def background_sigtest():
        mark = len(log)
        send(b"sigtest &\r")
        ok = wait_for("ready", 10)
        found = re.findall(r"sigtest (\d+) ready", log[mark:])
        return (found[-1] if ok and found else "0"), mark

    pid, mark = background_sigtest()
    check("a program runs in the background while the shell stays usable",
          "running in the background as process " + pid in log[mark:]
          and run("echo shell-is-free", "shell-is-free\r\n") and wait_for("rdx> ", 5))
    check("kill asks a program to stop, and it cleans up",
          run(f"kill {pid}", "caught signal 15", 5) and wait_for("sigtest finished cleanly", 5))
    wait_for("rdx> ", 2)
    pid, mark = background_sigtest()
    run(f"kill -9 {pid}", "rdx> ", 5)
    time.sleep(0.5)
    run("ps", "sh.exe")
    check("kill -9 cannot be caught",
          "caught signal" not in log[mark:] and "sigtest" not in log[log.rindex(" PID  TID"):])
    pid, mark = background_sigtest()
    check("the kill program sends signals too (and to a pipeline)",
          run(f"kill.exe {pid} | cat", "caught signal 15", 5) and wait_for("sigtest finished cleanly", 5))
    wait_for("rdx> ", 2)

    # ---- graphics: the screen, keyboard and mouse for one program ----
    check("a program can take over the display and draw",
          run("gfxtest", "pixel check ok", 15) and wait_for("rdx> ", 5))
    check("the console takes the screen back when a graphical program crashes",
          run("gfxtest crash", "crashed in user mode", 15) and wait_for("rdx> ", 5)
          and run("cat /proc/display", "in use by the text console") and wait_for("rdx> ", 5)
          and run("echo console-ok", "console-ok\r\n") and wait_for("rdx> ", 5))

    def mouse(*commands):
        for command in commands:
            mon.sendall(command.encode() + b"\n")
            time.sleep(0.06)

    send(b"paint\r")
    time.sleep(1.5)
    mouse("mouse_move -300 -200", "mouse_move 50 80", "mouse_button 1",
          *["mouse_move 15 6"] * 10, "mouse_button 0", "mouse_move 20 20")
    send(b"s")  # keys go to the program that has the display
    time.sleep(1.0)
    send(b"q")
    check("the PS/2 mouse draws in paint (moves, clicks, drags)",
          wait_for("paint: 1 stroke(s)", 10) and wait_for("pointer ended at 560,360", 5) and wait_for("rdx> ", 5))
    check("paint saved its picture",
          run("ls -l paint.ppm", "paint.ppm") and wait_for("rdx> ", 5)
          and run("cat paint.ppm | head -1", "P6\r\n") and wait_for("rdx> ", 5))
    check("the mouse is reported in /proc/display",
          run("cat /proc/display", "mouse:   working") and wait_for("rdx> ", 5))

    # ---- the desktop: window server, terminal window, apps ----
    def click_at(x, y, button=1):
        # Park the pointer in the top-left corner, then move to (x, y). In
        # steps: QEMU's PS/2 mouse only queues a few packets, and holds back
        # the rest of a big move to add to whatever comes next.
        steps = ["mouse_move -200 -200"] * 8
        while x > 0 or y > 0:
            sx, sy = min(x, 200), min(y, 200)
            steps.append(f"mouse_move {sx} {sy}")
            x, y = x - sx, y - sy
        mouse(*steps, f"mouse_button {button}", "mouse_button 0")
        time.sleep(0.4)

    def type_keys(text):
        send(text.encode())
        time.sleep(0.8)

    check("the desktop starts and opens a terminal window",
          run("desktop terminal", "desktop: running on a", 15))
    time.sleep(2.5)
    # Keys now go to the desktop, which passes them to the focused window:
    # the terminal, whose shell writes these files.
    type_keys("echo typed-in-a-window > winfile.txt\r")
    type_keys("desktop > second.txt\r")
    click_at(1100, 200)  # the background: no window has the keyboard...
    type_keys("m")        # ...so keys are shortcuts: m opens the monitor
    click_at(1100, 200)
    type_keys("f")        # and f the file manager
    time.sleep(1.5)
    click_at(130, 300)    # back into the terminal (the part not covered)
    type_keys("ps > ps-inside.txt\r")
    type_keys("open sleep 100000\r")  # a text program: gets a terminal window of its own
    time.sleep(1.5)
    click_at(130, 300)    # back into the first terminal
    type_keys("ps > ps-open.txt\r")
    click_at(748, 100)    # the first terminal's close box
    time.sleep(1.0)
    click_at(498, 783)    # Log out
    check("logging out returns to the console",
          wait_for("desktop: logged out", 10) and wait_for("rdx> ", 5))
    check("typing in the terminal window reaches its shell",
          run("cat winfile.txt", "typed-in-a-window\r\n") and wait_for("rdx> ", 5))
    check("a second desktop is refused (the name is taken)",
          run("cat second.txt", "already listening") and wait_for("rdx> ", 5))
    run("cat ps-open.txt", "rdx> ")
    wait_for("rdx> ", 5)
    opened = log[log.rindex("cat ps-open.txt"):]
    check("open gives a text program a terminal window of its own",
          opened.count("terminal.exe") == 2 and "sleep.exe" in opened)
    check("taskbar shortcuts started the monitor and the file manager",
          run("cat ps-inside.txt", "files.exe", 5) and "monitor.exe" in log[log.rindex("cat ps-inside.txt"):]
          and "terminal.exe" in log[log.rindex("cat ps-inside.txt"):] and wait_for("rdx> ", 5))
    run("ps", "sh.exe")
    wait_for("rdx> ", 5)
    listing = log[log.rindex(" PID  TID"):]
    check("closing windows and logging out leaves nothing running",
          not any(name in listing for name in ["terminal.exe", "desktop.exe", "monitor.exe", "files.exe", "sleep.exe"])
          and listing.count("sh.exe") == 1)
    check("the console has the screen back", run("cat /proc/display", "in use by the text console")
          and wait_for("rdx> ", 5))

    # ---- the browser, on pages stored on RDX ----
    check("the browser lays out a local page (headings, lists, links)",
          run("browser --dump /home.html", "Links:", 15) and "Title: RDX Browser" in log[log.rindex("browser --dump /home.html"):]
          and " * info.cern.ch[1] - the very first web site" in log[log.rindex("browser --dump /home.html"):]
          and wait_for("6. /help.html", 5) and wait_for("rdx> ", 5))
    check("the browser decodes entities and follows relative links",
          run("browser --dump help.html", "start page[1]", 15) and "cafe shows as cafe" in log[log.rindex("browser --dump help.html"):]
          and "&entities; and UTF-8" in log[log.rindex("browser --dump help.html"):]
          and wait_for("1. home.html", 5) and wait_for("rdx> ", 5))
    check("with no network, the browser says why a page cannot be opened",
          run("browser --dump https://example.com/", "Could not find the address of example.com", 15)
          and wait_for("rdx> ", 5))

    # ---- the browser's CSS, and the cryptography under https ----
    run("browser --dump csstest.html", "Title: CSS test", 15)
    wait_for("rdx> ", 10)
    css = log[log.rindex("browser --dump csstest.html"):]
    check("CSS hides elements by class, id, style attribute and @media",
          "visible note" in css and "shown on screens" in css and "HIDDEN" not in css)
    check("CSS: of two equal rules the later wins", "the later rule wins" in css)
    check("CSS centres text and narrow columns",
          re.search(r"\n {40,}middle\r?\n", css) is not None
          and re.search(r"\n {30,}a narrow centred", css) is not None)
    check("CSS list-style: none drops the bullet",
          re.search(r"\n +no bullet here", css) is not None and "* no bullet" not in css and "* with a bullet" in css)

    # ---- the layout engine and JavaScript ----
    mark = len(log)
    run("browser --dump layout.html", "Links:", 20)
    wait_for("rdx> ", 10)
    page = log[mark:]
    check("layout: flexbox puts items side by side, grid makes columns, floats let text beside them",
          re.search(r"FLEX-ONE first column +FLEX-TWO second column +FLEX-THREE third", page) is not None
          and re.search(r"GRID-A +GRID-B +GRID-C", page) is not None
          and re.search(r"FLOAT-LEFT box\s+Text that flows beside", page) is not None)
    check("layout: tables, positioned boxes and form controls",
          re.search(r"alpha +TABLE-ONE", page) is not None and re.search(r"in the box +CORNER", page) is not None
          and "[hello]" in page and "[x]" in page and "[two]" in page and "[Go]" in page)
    check("JavaScript: the language and the DOM (34 checks in the page)",
          run("set BROWSER_CONSOLE=1", "rdx> ") and run("browser --dump jstest.html", "JS TEST OK (34 checks)", 30)
          and wait_for("rdx> ", 10))
    page = log[log.rindex("browser --dump jstest.html"):]
    check("scripts change the page: added list items, innerHTML, hidden elements, no <noscript>",
          "three (added by a script)" in page and "Written by JavaScript" in page and "THIS SHOULD BE HIDDEN" not in page
          and "NOSCRIPT CONTENT" not in page and "PASS event loop order" in page)
    check("a page reads its address's query (local file, ?query, URLSearchParams)",
          run("browser --dump formresult.html?who=Ada", "You sent: name=Ada, news=no, colour=null", 20) and wait_for("rdx> ", 10))
    check("the garbage collector frees memory and keeps what is still used",
          run("browser --dump gctest.html", "GC TEST OK", 40) and wait_for("rdx> ", 10))
    check("a script that never ends is stopped, deep recursion is caught, and the page still shows",
          run("browser --dump jsbad.html", "after the bad scripts", 40) and wait_for("rdx> ", 10)
          and "the script ran too long and was stopped" in log[log.rindex("browser --dump jsbad.html"):]
          and "RangeError Maximum call stack size exceeded" in log[log.rindex("browser --dump jsbad.html"):])
    check("with scripts turned off, <noscript> shows instead",
          run("set BROWSER_NOJS=1", "rdx> ") and run("browser --dump jstest.html", "NOSCRIPT CONTENT", 20)
          and wait_for("rdx> ", 10) and "NOSCRIPT CONTENT" in log[log.rindex("browser --dump jstest.html"):]
          and run("unset BROWSER_NOJS", "rdx> ") and run("unset BROWSER_CONSOLE", "rdx> "))
    check("cryptography matches the standards' test vectors (SHA-256, HMAC, X25519, ChaCha20-Poly1305, AES-GCM)",
          run("tlstest", "CRYPTO OK", 30) and wait_for("rdx> ", 5))
    if HAVE_CERTS:
        digests = subprocess.run([sys.executable, "-c",
                                  "import hashlib,sys;d=open(sys.argv[1],'rb').read();"
                                  "print(hashlib.sha384(d).hexdigest(), hashlib.sha512(d).hexdigest())",
                                  os.path.join(CERTS, "sigdata.bin")], capture_output=True, text=True).stdout.split()
        check("SHA-384 and SHA-512 match Python's",
              run("certcheck --hash sigdata.bin", digests[1]) and wait_for("rdx> ", 5) and digests[0] in log[-2000:])
        sigs_ok = True
        for cert, scheme, sig in [("leaf_rsa.pem", "rsa-sha256", "sig_pkcs1.bin"), ("leaf_rsa.pem", "rsa-pss-sha256", "sig_pss256.bin"),
                                  ("leaf_rsa.pem", "rsa-pss-sha384", "sig_pss384.bin"), ("leaf_ec.pem", "ecdsa-sha256", "sig_ec256.bin"),
                                  ("ca_ec.pem", "ecdsa-sha384", "sig_ec384.bin")]:
            sigs_ok = sigs_ok and run(f"certcheck --signature {cert} {scheme} sigdata.bin {sig}", "SIGNATURE OK", 20) \
                and wait_for("rdx> ", 5)
        check("signatures made by openssl check out (RSA PKCS#1, RSA-PSS, ECDSA P-256 and P-384)", sigs_ok)
        check("a signature over changed data does not",
              run("certcheck --signature leaf_ec.pem ecdsa-sha256 sigdata2.bin sig_ec256.bin", "SIGNATURE BAD", 20)
              and wait_for("rdx> ", 5)
              and run("certcheck --signature leaf_rsa.pem rsa-pss-sha256 sigdata2.bin sig_pss256.bin", "SIGNATURE BAD", 20)
              and wait_for("rdx> ", 5))
        check("certificate chains: trusted roots, intermediates, wildcards and IP addresses",
              run("certcheck chain_rsa.pem test.rdx", "TRUSTED for test.rdx: issued by RDX Test RSA Authority", 20)
              and wait_for("rdx> ", 5)
              and run("certcheck chain_ec.pem x.wild.rdx", "TRUSTED for x.wild.rdx", 20) and wait_for("rdx> ", 5)
              and run("certcheck chain_ec.pem 10.0.2.2", "TRUSTED for 10.0.2.2", 20) and wait_for("rdx> ", 5))
        check("bad chains are refused, saying why",
              run("certcheck chain_ec_nomid.pem test.rdx", "which this machine does not trust", 20) and wait_for("rdx> ", 5)
              and run("certcheck other.pem test.rdx", "the certificate is for elsewhere.example, not this one", 20)
              and wait_for("rdx> ", 5)
              and run("certcheck chain_ec.pem a.b.wild.rdx", "not this one", 20) and wait_for("rdx> ", 5)
              and run("certcheck self.pem test.rdx", "self-signed", 20) and wait_for("rdx> ", 5))

    # ---- poll and pseudo-terminals ----
    check("poll: time limits, wake-ups, hang-ups, many descriptors",
          run("polltest", "POLL OK", 20) and wait_for("rdx> ", 5))
    check("a shell runs inside a pseudo-terminal and thinks it is on a terminal",
          run("ptyrun", "pseudo-terminal", 10) and wait_for("rdx> ", 10)
          and run("tty", "a terminal\r\n") and wait_for("rdx> ", 5))
    check("pipes, directories and screen control work inside it",
          run("ls /proc | wc -l", "10\r\n") and wait_for("rdx> ", 5)
          and run("cd /proc", "rdx/proc> ") and run("cd /", "rdx> ")
          and run("top -n 1", "ptyrun.exe", 15) and wait_for("rdx> ", 5))
    send(b"sleep 100000\r")
    time.sleep(0.5)
    send(b"\x03")
    check("Ctrl+C reaches the program running inside the pseudo-terminal",
          wait_for("[sleep.exe was terminated]", 5) and wait_for("rdx> ", 5))
    send(b"sigtest\r")
    caught = wait_for("ready", 10)
    for n in (1, 2, 3):
        send(b"\x03")
        caught = caught and wait_for(f"caught interrupt {n} of 3", 5)
    check("signal handlers work inside it too", caught and wait_for("finished cleanly", 5) and wait_for("rdx> ", 5))
    check("exit inside it returns to the outer shell",
          run("exit", "[ptyrun: sh finished with code 0]", 10) and wait_for("rdx> ", 5)
          and run("ptyrun echo one-shot", "one-shot\r\n") and wait_for("rdx> ", 5))

    # ---- environment variables, PATH, scripts ----
    check("shell variables expand in commands",
          run("set GREETING=hello there", "rdx> ") and run("echo [$GREETING] [${GREETING}!]", "[hello there] [hello there!]\r\n")
          and wait_for("rdx> ", 5))
    check("programs inherit the environment",
          run("env GREETING", "hello there\r\n") and wait_for("rdx> ", 5)
          and run("env | grep PATH", "PATH=/\r\n") and wait_for("rdx> ", 5))
    check("$? holds the last exit code",
          run("env NO_SUCH_VARIABLE", "exited with code 1") and wait_for("rdx> ", 5)
          and run("echo status=$?", "status=1\r\n") and wait_for("rdx> ", 5))
    check("unset removes a variable",
          run("unset GREETING", "rdx> ") and run("echo [$GREETING]", "[]\r\n") and wait_for("rdx> ", 5))
    check("PATH decides where programs are found",
          run("mkdir tools", "rdx> ") and run("cp hello.exe tools/greet.exe", "rdx> ")
          and run("greet", "unknown command or program: greet") and wait_for("rdx> ", 5)
          and run("set PATH=/:/tools", "rdx> ") and run("greet a", "argument 1: a") and wait_for("rdx> ", 5)
          and run("set PATH=/", "rdx> ") and run("rm -r tools", "rdx> "))
    check("source runs a file of commands",
          run("echo echo from-a-script > cmds.rc", "rdx> ") and run("echo # a comment >> cmds.rc", "rdx> ")
          and run("echo set SCRIPTED=yes >> cmds.rc", "rdx> ")
          and run("source cmds.rc", "from-a-script\r\n") and wait_for("rdx> ", 5)
          and run("echo scripted=$SCRIPTED", "scripted=yes\r\n") and wait_for("rdx> ", 5))
    check("the shipped startup.rc ran at boot", run("echo editor=$EDITOR", "editor=edit\r\n") and wait_for("rdx> ", 5))
    run("write startup.rc set BOOTVAR=set-at-boot", "rdx> ")  # for the reboot check

    # ---- clock, timestamps and system tools ----
    stamp = r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d"
    run("date", "rdx> ")
    wait_for("rdx> ", 5)
    year = re.findall(r"date\r\n(\d{4})-\d\d-\d\d \d\d:\d\d:\d\d\r\n", log)
    check(f"the clock reads a sensible date ({year[-1] if year else '?'})",
          bool(year) and int(year[-1]) == time.gmtime().tm_year)
    run("date.exe -s | cat", "\r\n")
    wait_for("rdx> ", 5)
    seconds = re.findall(r"\r\n(\d{9,11})\r\n", log)
    check("programs get the time in seconds since 1970",
          bool(seconds) and abs(int(seconds[-1]) - time.time()) < 86400 * 2)
    run("ls -l note.txt", "1 file(s)")
    wait_for("rdx> ", 5)
    note_time = re.findall(r"(" + stamp + r")  note\.txt", log)
    check(f"files carry a modification time ({note_time[-1] if note_time else 'none'})", bool(note_time))
    check("system tools: uname, df, free, du, top",
          run("uname -a", "x86_64, 1 CPU core(s)") and wait_for("rdx> ", 5)
          and run("df", "KiB free") and wait_for("rdx> ", 5)
          and run("free", "the heap the kernel keeps") and wait_for("rdx> ", 5)
          and run("du saved_dir", "saved_dir\r\n") and wait_for("rdx> ", 5)
          and run("top -n 1", "top.exe", 15) and "CPU%" in log and wait_for("rdx> ", 5))


    # ---- c-comp: the C compiler that runs inside RDX ----
    check("c-comp compiles sample.c", run("c-comp sample.c", "sample.exe:", 60) and wait_for("rdx> ", 10))
    check("the compiled program runs", run("sample 7", "Longest: 7", 20) and "7 takes  16 steps" in log
          and wait_for("rdx> ", 10))
    check("c-comp passes its own test suite",
          run("c-comp ctest.c", "ctest.exe:", 60) and wait_for("rdx> ", 10)
          and run("ctest", "CHECKS PASSED", 30) and "FAILED" not in log and wait_for("rdx> ", 10))
    check("c-comp builds hello.c unchanged",
          run("c-comp t_hello.c", "t_hello.exe:", 60) and wait_for("rdx> ", 10)
          and run("t_hello x y", "argument 2: y", 20) and wait_for("rdx> ", 10))
    check("c-comp builds primes.c unchanged, right answer",
          run("c-comp t_primes.c", "t_primes.exe:", 60) and wait_for("rdx> ", 10)
          and run("t_primes 1000000", "78498 primes below 1000000", 60) and wait_for("rdx> ", 10))
    check("c-comp builds wc.c and grep.c unchanged, and they work in a pipeline",
          run("c-comp t_wc.c", "t_wc.exe:", 60) and wait_for("rdx> ", 10)
          and run("c-comp t_grep.c", "t_grep.exe:", 60) and wait_for("rdx> ", 10)
          and run("seq 30 | t_grep 2 | t_wc -l", "12\r\n", 20) and wait_for("rdx> ", 10))
    check("c-comp reports errors with file and line",
          run("write bad.c int main(void) { return nope; }", "rdx> ")
          and run("c-comp bad.c", "bad.c:1: error: 'nope' is not declared") and wait_for("rdx> ", 10)
          and run("rm bad.c", "rdx> "))

    time.sleep(0.5)
    after = used_memory()
    check(f"process memory is freed on exit ({before} MiB before, {after} MiB after)", before == after)

    # ---- disk ----
    check("disk command reports the SATA disk", run("disk", "saves so far"))
    check("a big file (256 KiB) can be saved",
          run("rx", "rx> ") and run('let s = "0123456789abcdef"', "rx> ")
          and run("for i in 0..14 { s = s + s }", "rx> ", 30)
          and run('save("big.txt", s)', "1\r\n", 30) and run("exit()", "rdx> "))
    check("mv renames, cp copies",
          run("write a.txt rename me", "rdx> ") and run("mv a.txt b.txt", "rdx> ")
          and run("cat a.txt", "no such file") and run("cp b.txt c.txt", "rdx> ")
          and run("cat b.txt", "rename me") and run("cat c.txt", "rename me")
          and run("mv b.txt c.txt", "already exists"))
    check("rm + ls", run("rm bad.rx", "rdx> ") and run("rm bad2.rx", "rdx> ") and run("ls", "note.txt"))

    # Real PS/2 key presses, including a shifted one, through the keyboard IRQ.
    time.sleep(0.3)
    for key in ["e", "c", "h", "o", "spc", "shift-k", "b", "ret"]:
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.15)
    check("PS/2 keyboard input", wait_for("Kb\r\n", 5))

    if os.path.exists(SHOT):
        os.remove(SHOT)
    shot_arg = SHOT.replace("\\", "/")
    mon.sendall(f'screendump "{shot_arg}"\n'.encode())
    time.sleep(1.0)
    check("screenshot captured", os.path.exists(SHOT) and os.path.getsize(SHOT) > 1000)
    stop_qemu()

    # ================================================================ second boot
    mark = len(log)
    start_qemu()
    check("reboots to shell prompt", wait_for("rdx> ", 60))
    boot2 = log[mark:]
    restored = re.search(r"(\d+) saved file\(s\) restored", boot2)
    check(f"saved files restored after reboot ({restored.group(1) if restored else 'none'})",
          bool(restored) and int(restored.group(1)) >= 5)
    check("edited file survived the reboot", run("cat note.txt", "Xhello\r\nworld\r\n"))
    check("a directory and its file survived the reboot",
          run("ls saved_dir", "kept.txt\r\n") and wait_for("rdx> ", 5)
          and run("cat saved_dir/kept.txt", "directories persist\r\n") and wait_for("rdx> ", 5))
    check("program-written file survived (guess.best)", run("cat guess.best", "rdx> ") and "no such file" not in log[mark:])
    check("script survived and still runs", run("rx t.rx", "42 [1, \"two\"]\r\n"))
    check("deleted file stayed deleted", run("cat bad.rx", "no such file"))
    check("a program compiled before the reboot still runs", run("ctest", "CHECKS PASSED", 30) and wait_for("rdx> ", 10))
    check("big file survived intact",
          run("rx", "rx> ") and run('let s = read("big.txt")', "rx> ", 30)
          and run('[len(s), substr(s, 262128, 16)]', '[262144, "0123456789abcdef"]', 30)
          and run("exit()", "rdx> "))
    run("ls -l note.txt", "1 file(s)")
    wait_for("rdx> ", 5)
    after_reboot = re.findall(r"(" + stamp + r")  note\.txt", log[mark:])
    check("modification times survived the reboot", bool(note_time) and after_reboot == note_time[-1:])
    check("a startup.rc written by the user ran at boot",
          run("echo bootvar=$BOOTVAR", "bootvar=set-at-boot\r\n") and wait_for("rdx> ", 5))
    # Break the shell on purpose: a user file named sh.exe hides the real one.
    # init must notice it cannot run, and fall back to the kernel's own
    # rescue shell, from which it can be repaired.
    check("a broken sh.exe drops into the kernel's rescue shell",
          run("write sh.exe this is not a program", "rdx> ") and run("exit", "rescue shell", 15)
          and wait_for("rdx> ", 5) and run("echo rescued", "rescued\r\n") and wait_for("rdx> ", 5))
    check("the rescue shell can repair it",
          run("rm sh.exe", "rdx> ") and run("ls sh.exe", "sh.exe") and wait_for("rdx> ", 5))
    # Power off the proper way: through ACPI. QEMU itself then exits.
    send(b"shutdown\r")
    powered_off = wait_for("ACPI: powering off (S5", 10)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        powered_off = False
    check("shutdown powers the machine off through ACPI", powered_off)
    stop_qemu()

    # ================================================================ third boot
    # A machine with a USB keyboard on a USB 3 controller and neither of the
    # usual reference clocks (no HPET, no legacy PIT).
    mark = len(log)
    start_qemu("q35,hpet=off,pit=off",
               ["-smp", "4", "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,id=kbd0", "-device", "usb-mouse",
                "-netdev", f"user,id=net0,hostfwd=tcp:127.0.0.1:{FORWARD_PORT}-:8080",
                "-device", "e1000,netdev=net0"])
    check("boots without HPET or PIT", wait_for("rdx> ", 60) and "calibrated against the ACPI timer" in log[mark:])
    check("all 4 CPU cores start", "4 CPU core(s) running" in log[mark:])
    # Check the GUEST's own clock, not host wall-clock: under host CPU load
    # QEMU runs slower than real time, which would wrongly fail a wall-clock
    # test even though the kernel's timer is perfectly accurate.
    def guest_uptime():
        mark = len(log)
        if not (run("uptime", "ticks)") and wait_for("rdx> ", 5)):
            return -1
        return int(re.findall(r"\((\d+) ticks\)", log[mark:])[-1])

    before_ticks = guest_uptime()
    slept = run("sleep 3000", "rdx> ", 30)
    after_ticks = guest_uptime()
    elapsed_ticks = after_ticks - before_ticks
    check(f"timer is accurate without HPET or PIT (3 s sleep = {elapsed_ticks} ticks)",
          slept and before_ticks >= 0 and after_ticks >= 0 and 295 <= elapsed_ticks <= 320)

    def keys(sequence):
        for key in sequence:
            mon.sendall(f"sendkey {key}\n".encode())
            time.sleep(0.12)

    # ---- network: address by DHCP, then ping the virtual gateway ----
    check("network adapter found", "Intel Ethernet (device 100e)" in log[mark:])
    time.sleep(1.0)
    check("address obtained by DHCP", run("net", "packets:") and wait_for("rdx> ", 5)
          and "address:  10.0.2.15 (mask 255.255.255.0)" in log[mark:] and "gateway:  10.0.2.2" in log[mark:])
    check("ping gets four replies from the gateway", run("ping 10.0.2.2", "4 sent, 4 received", 15))
    check("an address nobody has is reported unreachable",
          run("ping 10.0.2.77", "4 sent, 0 received", 20) and "no route to 10.0.2.77" in log[mark:])

    # ---- TCP, UDP and name lookup, from programs ----
    check("TCP: fetch a small page from a web server (wget)",
          run(f"wget http://10.0.2.2:{WEB_PORT}/small.txt", "hello from the host\r\n", 20) and wait_for("rdx> ", 10))
    check("TCP: download 190 KiB intact",
          run(f"wget http://10.0.2.2:{WEB_PORT}/big.txt big.txt", f"saved {len(BIG_TEXT)} bytes as big.txt", 60)
          and wait_for("rdx> ", 10)
          and run("wc -l big.txt", "20000\r\n") and wait_for("rdx> ", 10)
          and run("tail -1 big.txt", "line 19999\r\n") and wait_for("rdx> ", 10)
          and run("grep -c 7777 big.txt", "2\r\n") and wait_for("rdx> ", 10))
    check("TCP: send through a pipe and read the reply (nc)",
          run(f"echo ping pong | nc 10.0.2.2 {ECHO_PORT}", "you said: ping pong", 20) and wait_for("rdx> ", 10))
    check("TCP: a refused connection is reported, not hung",
          run("wget http://10.0.2.2:1/x", "could not connect", 30) and wait_for("rdx> ", 10))
    check("UDP: send a packet and get the reply",
          run(f"udpsend 10.0.2.2 {UDP_PORT} hello there", "replied (17 bytes): echo: hello there", 20)
          and wait_for("rdx> ", 10))
    browse_mark = len(log)
    check("the browser fetches a page over HTTP, following a redirect",
          run(f"browser --dump http://10.0.2.2:{WEB_PORT}/old", "Links:", 30) and wait_for("rdx> ", 10)
          and "Title: Host page" in log[browse_mark:] and "Served by the host" in log[browse_mark:])
    page_text = log[browse_mark:]
    check("it decodes the page, runs its script (document.write) and shows no code or styles",
          "Fish & chips - cafe bold" in page_text and "written by a script" in page_text
          and "document.write" not in page_text and "color:red" not in page_text)
    check("relative links are made absolute against the redirected address",
          f"1. http://10.0.2.2:{WEB_PORT}/dir/other.html" in page_text
          and f"2. http://10.0.2.2:{WEB_PORT}/top.html" in page_text)
    if HTTPS_PORT:
        mark_tls = len(log)
        check("https: the browser fetches a page over TLS 1.3 (redirect included)",
              run(f"browser --dump https://10.0.2.2:{HTTPS_PORT}/old", "Links:", 30) and wait_for("rdx> ", 10)
              and "Served by the host" in log[mark_tls:] and "encrypted (TLS 1.3" in log[mark_tls:]
              and "identity verified by RDX Test RSA Authority" in log[mark_tls:]
              and f"1. https://10.0.2.2:{HTTPS_PORT}/dir/other.html" in log[mark_tls:])
        mark_tls = len(log)
        check("https: an EC certificate through an intermediate authority is verified (ECDSA)",
              run(f"wget https://10.0.2.2:{HTTPS_EC_PORT}/big.txt ec.txt", "saved", 30) and wait_for("rdx> ", 10)
              and "identity verified by RDX Test Intermediate" in log[mark_tls:])
        tls12_ok = True
        for port, cipher, name, who in [(HTTPS_12_PORT, "aes", "AES-128-GCM", "RDX Test RSA Authority"),
                                        (HTTPS_12_EC_PORT, "chacha", "ChaCha20-Poly1305", "RDX Test Intermediate")]:
            mark_tls = len(log)
            tls12_ok = tls12_ok and run(f"set TLS_CIPHERS={cipher}", "rdx> ") \
                and run(f"wget https://10.0.2.2:{port}/big.txt t12.txt", "saved", 60) and wait_for("rdx> ", 10) \
                and f"TLS 1.2, {name}), identity verified by {who}" in log[mark_tls:] \
                and f"saved {len(BIG_TEXT)} bytes" in log[mark_tls:] \
                and run("tail -1 t12.txt", "line 19999\r\n") and wait_for("rdx> ", 10)
        run("unset TLS_CIPHERS", "rdx> ")
        check("TLS 1.2: RSA and ECDSA servers, AES-GCM and ChaCha20, verified, downloads intact", tls12_ok)
        mark_tls = len(log)
        check("https: a self-signed certificate is refused, and the browser explains",
              run(f"wget https://10.0.2.2:{HTTPS_BAD_PORT}/big.txt bad.txt", "untrusted certificate: the certificate is self-signed", 30)
              and wait_for("rdx> ", 10)
              and run(f"browser --dump https://10.0.2.2:{HTTPS_BAD_PORT}/", "Go on anyway (not safe)", 30)
              and wait_for("rdx> ", 10))
        mark_tls = len(log)
        check("TLS_INSECURE=1 connects anyway, and says the identity is not verified",
              run("set TLS_INSECURE=1", "rdx> ")
              and run(f"wget https://10.0.2.2:{HTTPS_BAD_PORT}/big.txt bad.txt", "saved", 30) and wait_for("rdx> ", 10)
              and "site identity NOT verified: the certificate is self-signed" in log[mark_tls:]
              and run("unset TLS_INSECURE", "rdx> "))
        for cipher, name in [("aes", "AES-128-GCM"), ("chacha", "ChaCha20-Poly1305")]:
            mark_tls = len(log)
            check(f"https with {name}: a 190 KiB download arrives intact",
                  run(f"set TLS_CIPHERS={cipher}", "rdx> ")
                  and run(f"wget https://10.0.2.2:{HTTPS_PORT}/big.txt big2.txt", "saved", 60) and wait_for("rdx> ", 10)
                  and name in log[mark_tls:] and f"saved {len(BIG_TEXT)} bytes" in log[mark_tls:]
                  and run("tail -1 big2.txt", "line 19999\r\n") and wait_for("rdx> ", 10))
        run("unset TLS_CIPHERS", "rdx> ")
    else:
        print("SKIP  https against a local server (openssl not found)")
    check("404 from the server is reported",
          run(f"wget http://10.0.2.2:{WEB_PORT}/missing out.txt", "404", 20) and wait_for("rdx> ", 10))

    # RDX as the server: start its web server and connect in from this PC.
    send(b"httpd\r")
    serving = wait_for("serving files at http://10.0.2.15:8080/", 10)

    def fetch(path):
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{FORWARD_PORT}{path}", timeout=30) as response:
                return response.read()
        except Exception as error:  # noqa: BLE001
            return repr(error).encode()

    check("TCP server: RDX accepts a connection and serves a file",
          serving and b"Welcome to RDX" in fetch("/readme.txt"))
    check("TCP server: RDX sends 190 KiB intact", fetch("/big.txt") == BIG_TEXT)
    check("TCP server: file listing and 404", b"hello.exe" in fetch("/") and b"404" in fetch("/nope"))
    check("TCP server: several connections one after another",
          all(b"Welcome to RDX" in fetch("/readme.txt") for _ in range(5)))
    send(b"\x03")
    stop_mark = len(log)
    check("the server shuts down cleanly on Ctrl+C",
          wait_for("httpd: shutting down", 10) and wait_for("rdx> ", 10) and "was terminated" not in log[stop_mark:])
    time.sleep(1.0)
    run("net", "packets:")
    wait_for("rdx> ", 5)
    check("no sockets left open afterwards", "socket:" not in log[log.rindex("adapter:"):])

    # Name lookup needs a real DNS server, i.e. this PC being online.
    check("name lookup of a literal address", run("nslookup 10.0.2.2", "10.0.2.2 has address 10.0.2.2")
          and wait_for("rdx> ", 10))
    lookup_mark = len(log)
    run("nslookup example.com", "example.com", 15)
    wait_for("rdx> ", 10)
    if "example.com has address" in log[lookup_mark:]:
        check("name lookup over DNS (example.com)", True)
        check("fetch a real web page by name",
              run("wget http://example.com/", "Example Domain", 30) and wait_for("rdx> ", 15))
        check("fetch a real web page over https",
              run("browser --dump https://example.com/", "Title: Example Domain", 30) and wait_for("rdx> ", 15))
    else:
        print("SKIP  name lookup over DNS (this PC appears to be offline)")

    # ---- multi-core: threads in parallel, and everything else still right ----
    threads_ok = run("threads", "shared counter: 200000 (expected 200000) OK", 60) and wait_for("rdx> ", 10)
    speedup = re.findall(r"speed-up: ([\d.]+)x", log[mark:])
    check(f"threads run in parallel on 4 cores (speed-up {speedup[-1] if speedup else '?'}x)",
          threads_ok and bool(speedup) and float(speedup[-1]) >= 2.0)
    check("many threads on few cores still give the right answer",
          run("threads 16", "shared counter: 800000 (expected 800000) OK", 60) and "MISMATCH" not in log
          and wait_for("rdx> ", 10))
    check("compiler and its test suite work on 4 cores",
          run("c-comp ctest.c", "ctest.exe:", 60) and wait_for("rdx> ", 10)
          and run("ctest", "CHECKS PASSED", 30) and wait_for("rdx> ", 10))
    check("pipelines and shared memory on 4 cores",
          run("seq 3000 | sort -rn | head -1", "3000\r\n", 30) and wait_for("rdx> ", 10)
          and run("yes | head -500 | wc -l", "500\r\n", 30) and wait_for("rdx> ", 10)
          and run("shmdemo", "shared sum = 396 (expected 396)") and wait_for("rdx> ", 10))
    check("kernel tasks and a crashing program on 4 cores",
          run("spawn 6", "tick 5 of 5", 20) and run("evil", "crashed in user mode") and wait_for("rdx> ", 10)
          and run("rx demo", "checksum 599994", 60) and wait_for("rdx> ", 10))
    time.sleep(6)  # let the remaining workers finish
    run("ps", "sh.exe")
    check("no tasks left over afterwards", "worker" not in log[log.rindex(" PID  TID"):])

    time.sleep(1.5)  # the USB driver finds the keyboard in the background
    check("USB keyboard detected", run("usb", "keyboard ready"))
    check("USB mouse detected", run("usb", "mouse ready") and wait_for("rdx> ", 5))
    send(b"paint\r")
    time.sleep(1.5)
    for command in ["mouse_move 100 50", "mouse_button 1", "mouse_move 60 60", "mouse_move 60 60", "mouse_button 0"]:
        mon.sendall(command.encode() + b"\n")
        time.sleep(0.08)
    time.sleep(0.5)
    send(b"q")
    check("the USB mouse draws in paint",
          wait_for("paint: 1 stroke(s)", 10) and wait_for("pointer ended at 860,570", 5)
          and wait_for("rdx> ", 5))
    keys(["e", "c", "h", "o", "spc", "shift-u", "s", "b", "minus", "shift-1", "ret"])
    check("USB keyboard types (with shift)", wait_for("Usb-!\r\n", 5))
    # Left arrow then typing inserts nothing special in the shell, so test the
    # navigation keys where they matter: in the editor.
    send(b"edit usb.txt\r")
    time.sleep(1.0)
    keys(["b", "left", "a", "end", "c", "ctrl-s", "ctrl-q"])
    check("USB arrow keys and Ctrl work in the editor", wait_for("rdx> ", 10) and run("cat usb.txt", "abc\r\n"))

    # Pull the keyboard out and plug a different one in, while running.
    mon.sendall(b"device_del kbd0\n")
    time.sleep(2.0)
    check("unplugging is noticed", run("usb", "unplugged"))
    mon.sendall(b"device_add usb-kbd,id=kbd1,bus=xhci.0\n")
    time.sleep(3.0)
    keys(["e", "c", "h", "o", "spc", "shift-p", "l", "u", "g", "ret"])
    check("a keyboard plugged in later works", wait_for("Plug\r\n", 5))
    check("blocking wake-ups across 4 cores",
          run("pingpong 2000", "round trips in", 30) and wait_for("rdx> ", 10)
          and int(re.findall(r"\((\d+) microseconds each\)", log)[-1]) < 3000)
    # Restart through ACPI. QEMU was started with -no-reboot, so it exits
    # instead of actually booting again.
    send(b"reboot\r")
    restarted = wait_for("ACPI: restarting through the reset register", 10)
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        restarted = False
    check("reboot restarts the machine through ACPI", restarted)
    stop_qemu()

    # ================================================================ USB storage
    # No SATA disk this time: files are saved to a USB flash drive instead,
    # and must still be there after a reboot.
    usb_image = os.path.join(ROOT, "build", "test-usb.img")
    with open(usb_image, "wb") as f:
        f.write(b"RDXDISK1")
        f.truncate(32 * 1024 * 1024)
    stick = ["-device", "qemu-xhci", "-drive", f"if=none,id=stick,format=raw,file={usb_image}",
             "-device", "usb-storage,drive=stick"]

    mark = len(log)
    start_qemu(extra=stick, sata_disk=False)
    check("boots with only a USB drive", wait_for("rdx> ", 60))
    time.sleep(2.0)
    check("USB drive is mounted as the RDX disk", run("disk", "USB disk") and wait_for("rdx> ", 5))
    check("files can be written while it is attached",
          run("echo kept on a usb stick > stick.txt", "rdx> ") and run("seq 500 > numbers.txt", "rdx> "))
    time.sleep(2.0)  # the USB thread writes changes out in the background
    stop_qemu()

    mark = len(log)
    start_qemu(extra=stick, sata_disk=False)
    check("reboots with the USB drive", wait_for("rdx> ", 60))
    time.sleep(2.0)
    check("files saved on the USB drive survived the reboot",
          run("usb", "2 saved file(s) restored") and wait_for("rdx> ", 5)
          and run("cat stick.txt", "kept on a usb stick\r\n") and wait_for("rdx> ", 5)
          and run("wc -l numbers.txt", "500\r\n") and wait_for("rdx> ", 5))
    stop_qemu()
except Exception as e:  # noqa: BLE001
    check(f"harness error: {e!r}", False)
finally:
    time.sleep(0.3)
    if proc is not None and proc.poll() is None:
        proc.kill()

with open(os.path.join(ROOT, "build", "serial.log"), "w", encoding="utf-8", newline="") as f:
    f.write(log)

if os.path.exists(SHOT):
    try:
        from PIL import Image
        Image.open(SHOT).save(os.path.join(ROOT, "build", "screenshot.png"))
    except Exception:  # noqa: BLE001
        pass

print(f"\n{len(failures)} failure(s). Serial log: build/serial.log")
sys.exit(1 if failures else 0)
