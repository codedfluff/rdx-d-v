"""Boot RDX, run its web server, and throw hostile traffic at it from this PC.

Checks whether the kernel survives (stays responsive on the serial console)
after each round of abuse. Prints the serial log at the end.
"""
import os
import shutil
import socket
import subprocess
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESP = os.path.join(ROOT, "build", "esp")
DISK = os.path.join(ROOT, "build", "attack-disk.img")
SERIAL_PORT = 45570
HTTP_FWD = 48080
UDP_FWD = 48081

qemu = shutil.which("qemu-system-x86_64") or r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ovmf = os.path.join(os.path.dirname(qemu), "share", "edk2-x86_64-code.fd")

with open(DISK, "wb") as f:
    f.write(b"RDXDISK1")
    f.truncate(16 * 1024 * 1024)

proc = subprocess.Popen([
    qemu, "-machine", "q35", "-m", "256M", "-smp", "4",
    "-drive", f"if=pflash,format=raw,readonly=on,file={ovmf}",
    "-drive", f"format=raw,file=fat:rw:{ESP}",
    "-drive", f"format=raw,file={DISK}",
    "-netdev",
    f"user,id=n0,hostfwd=tcp:127.0.0.1:{HTTP_FWD}-:8080,hostfwd=udp:127.0.0.1:{UDP_FWD}-:9999",
    "-device", "e1000,netdev=n0",
    "-no-reboot", "-display", "none",
    "-serial", f"tcp:127.0.0.1:{SERIAL_PORT},server=on,wait=off",
])

log = ""


def recv_serial(sock, seconds):
    global log
    sock.settimeout(0.2)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            data = sock.recv(4096)
            if data:
                log += data.decode("utf-8", "replace")
        except socket.timeout:
            pass


def wait_for(sock, text, seconds):
    global log
    start = len(log)
    deadline = time.time() + seconds
    sock.settimeout(0.2)
    while time.time() < deadline:
        if text in log[start:]:
            return True
        try:
            data = sock.recv(4096)
            if data:
                log += data.decode("utf-8", "replace")
        except socket.timeout:
            pass
    return text in log[start:]


def banner(text):
    print(f"\n=== {text} ===")


def alive(serial, label):
    """Is RDX still alive? The web server is in the foreground, so instead of
    the shell we make a fresh HTTP request: a correct reply proves the kernel
    is scheduling, networking and serving files normally. Retries a few
    times: a single failed connect right after a flood is usually the host
    running out of ephemeral sockets, not RDX going down."""
    ok = False
    for attempt in range(5):
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_FWD), timeout=8)
            s.sendall(b"GET /loot.txt HTTP/1.0\r\n\r\n")
            s.settimeout(8)
            reply = b""
            while chunk := s.recv(4096):
                reply += chunk
            s.close()
            if b"secret data" in reply:
                ok = True
                break
        except OSError:
            pass
        time.sleep(1.0)
    note = "" if attempt == 0 else f" (after {attempt + 1} tries)"
    print(f"  [{'ALIVE' if ok else 'DEAD '}] kernel serves a correct reply after {label}{note}")
    return ok


serial = None
for _ in range(60):
    try:
        serial = socket.create_connection(("127.0.0.1", SERIAL_PORT), timeout=1)
        break
    except OSError:
        time.sleep(0.2)

try:
    if not wait_for(serial, "rdx> ", 60):
        print("RDX never reached the shell prompt")
        raise SystemExit

    # Make a target file and start the web server.
    serial.sendall(b"echo secret data the attacker should not be able to crash us over > loot.txt\r")
    wait_for(serial, "rdx> ", 5)
    serial.sendall(b"httpd\r")
    wait_for(serial, "serving files", 10)
    time.sleep(1.0)

    def tcp(payload, read_reply=True, port=HTTP_FWD):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=5)
            s.sendall(payload)
            if read_reply:
                s.settimeout(3)
                try:
                    while s.recv(4096):
                        pass
                except OSError:
                    pass
            s.close()
            return True
        except OSError as e:
            return f"({e})"

    banner("1. Normal request, as a baseline")
    tcp(b"GET /loot.txt HTTP/1.0\r\n\r\n")
    alive(serial, "a normal request")

    banner("2. Malformed request lines")
    for name, payload in [
        ("no method", b"/loot.txt\r\n\r\n"),
        ("garbage", b"\x00\x01\x02\xff\xfe random bytes not http\r\n\r\n"),
        ("giant path", b"GET /" + b"A" * 100000 + b" HTTP/1.0\r\n\r\n"),
        ("giant header", b"GET / HTTP/1.0\r\nX: " + b"B" * 200000 + b"\r\n\r\n"),
        ("no newline ever", b"GET / HTTP/1.0 and then nothing"),
        ("path traversal", b"GET /../../../etc/passwd HTTP/1.0\r\n\r\n"),
        ("null in path", b"GET /loot\x00.txt HTTP/1.0\r\n\r\n"),
        ("negative lengths", b"GET / HTTP/1.0\r\nContent-Length: -999999\r\n\r\n"),
    ]:
        result = tcp(payload)
        print(f"  sent: {name:18} -> {'connected' if result is True else result}")
    alive(serial, "malformed requests")

    banner("3. Slowloris: open a request and dribble it one byte at a time, never finishing")
    slow = []
    for _ in range(8):
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_FWD), timeout=5)
            s.sendall(b"GET / HTTP/1.0\r\n")
            slow.append(s)
        except OSError:
            pass
    print(f"  opened {len(slow)} half-finished connections")
    alive(serial, "slowloris")

    banner("4. Connection flood: open and drop 300 connections fast")
    opened = 0
    for _ in range(300):
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_FWD), timeout=2)
            s.close()
            opened += 1
        except OSError:
            pass
    print(f"  completed {opened} connect/close cycles")
    alive(serial, "connection flood")

    banner("5. Send a reply to an open port with no request at all, then reset")
    for _ in range(20):
        try:
            s = socket.create_connection(("127.0.0.1", HTTP_FWD), timeout=2)
            # abort with RST instead of a clean close
            s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, (1).to_bytes(2, "little") + (0).to_bytes(2, "little"))
            s.sendall(b"\xff" * 4096)
            s.close()
        except OSError:
            pass
    alive(serial, "garbage + RST")

    banner("6. UDP: blast the forwarded UDP port with junk of all sizes")
    us = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for size in [0, 1, 100, 1472, 2000, 9000, 60000]:
        try:
            us.sendto(b"\xde\xad\xbe\xef" + b"X" * max(0, size - 4), ("127.0.0.1", UDP_FWD))
        except OSError:
            pass
    for _ in range(500):
        try:
            us.sendto(os.urandom(512), ("127.0.0.1", UDP_FWD))
        except OSError:
            pass
    us.close()
    alive(serial, "UDP flood")

    banner("7. Close the half-finished connections and hammer the server again")
    for s in slow:
        try:
            s.close()
        except OSError:
            pass
    for _ in range(10):
        tcp(b"GET /loot.txt HTTP/1.0\r\n\r\n")
    alive(serial, "recovery")

    banner("8. Can the server still serve a correct reply after all that?")
    try:
        import urllib.request
        with urllib.request.urlopen(f"http://127.0.0.1:{HTTP_FWD}/loot.txt", timeout=10) as r:
            body = r.read()
        print(f"  server replied with {len(body)} bytes: {body[:60]!r}")
    except Exception as e:  # noqa: BLE001
        print(f"  server did NOT reply cleanly: {e!r}")

    recv_serial(serial, 2)
    final = alive(serial, "EVERYTHING")

    banner("9. Stop the server with Ctrl+C and check the shell still works")
    serial.sendall(b"\x03")
    wait_for(serial, "rdx> ", 8)
    serial.sendall(b"uptime\r")
    shell_ok = wait_for(serial, "ticks)", 8)
    print(f"  [{'ALIVE' if shell_ok else 'DEAD '}] shell responds after the server is stopped")
    serial.sendall(b"cat loot.txt\r")
    wait_for(serial, "rdx> ", 5)
    serial.sendall(b"mem\r")
    wait_for(serial, "rdx> ", 5)

    print(f"\n=== Verdict: kernel is {'STILL ALIVE' if final and shell_ok else 'DEAD'} ===")
finally:
    time.sleep(0.5)
    with open(os.path.join(ROOT, "build", "attack.log"), "w", encoding="utf-8", newline="") as f:
        f.write(log)
    proc.kill()
