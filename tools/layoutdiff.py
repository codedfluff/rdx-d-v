"""Compare RDX's layout of a page with headless Chrome's, element by element.

    python tools/layoutdiff.py URL [RDX_URL] [--width 800] [--wait 3] [--tol 3] [--show 40]

URL is what Chrome loads (e.g. http://127.0.0.1:8765/gridtest.html); RDX_URL
is what RDX loads (default: the same with 127.0.0.1 replaced by 10.0.2.2,
QEMU's address for this machine). Both lay the page out at the same width;
the elements under <body> are matched in document order by tag, and those
whose boxes differ by more than --tol pixels are listed.

Chrome is found in CHROME (env) or the PuppeteerSharp download. RDX runs
through tools/try.py with networking, so build first."""
import base64, difflib, json, os, re, socket, struct, subprocess, sys, tempfile, time, urllib.request

args = [a for a in sys.argv[1:]]


def option(name, default):
    if name in args:
        i = args.index(name)
        value = args[i + 1]
        del args[i:i + 2]
        return type(default)(value)
    return default


WIDTH = option("--width", 800)
WAIT = option("--wait", 3.0)
TOL = option("--tol", 3)
SHOW = option("--show", 40)
if not args:
    sys.exit(__doc__)
URL = args[0]
RDX_URL = args[1] if len(args) > 1 else URL.replace("127.0.0.1", "10.0.2.2").replace("localhost", "10.0.2.2")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

BOXES_JS = """(() => {
  const out = [];
  for (const e of [document.body, ...document.body.querySelectorAll('*')]) {
    const r = e.getBoundingClientRect();
    out.push([e.tagName.toLowerCase(), Math.round(r.left + scrollX), Math.round(r.top + scrollY),
              Math.round(r.width), Math.round(r.height), e.id || '-', (e.getAttribute('class') || '-').split(' ')[0] || '-']);
  }
  return JSON.stringify(out);
})()"""


def chrome_boxes():
    chrome = os.environ.get("CHROME") or os.path.join(os.environ.get("LOCALAPPDATA", ""),
                                                      r"PuppeteerSharp\Chrome\Win64-124.0.6367.201\chrome-win64\chrome.exe")
    port = 9334
    profile = tempfile.mkdtemp(prefix="layoutdiff")
    proc = subprocess.Popen([chrome, "--headless=new", f"--remote-debugging-port={port}", f"--user-data-dir={profile}",
                             "--hide-scrollbars", f"--window-size={WIDTH},800", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json"))
                break
            except Exception:
                time.sleep(0.1)
        page = [t for t in tabs if t["type"] == "page"][0]
        host_port, path = page["webSocketDebuggerUrl"][5:].split("/", 1)
        s = socket.create_connection(tuple([host_port.split(":")[0], int(host_port.split(":")[1])]))
        key = base64.b64encode(os.urandom(16)).decode()
        s.sendall((f"GET /{path} HTTP/1.1\r\nHost: {host_port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += s.recv(4096)
        rest = [buf.split(b"\r\n\r\n", 1)[1]]

        def read(n):
            while len(rest[0]) < n:
                rest[0] += s.recv(1 << 20)
            out, rest[0] = rest[0][:n], rest[0][n:]
            return out

        def recv():
            data = b""
            while True:
                h = read(2)
                ln = h[1] & 127
                if ln == 126:
                    ln = struct.unpack(">H", read(2))[0]
                elif ln == 127:
                    ln = struct.unpack(">Q", read(8))[0]
                data += read(ln)
                if h[0] & 0x80:
                    return json.loads(data)

        ids = [0]

        def send(method, params=None):
            ids[0] += 1
            raw = json.dumps({"id": ids[0], "method": method, "params": params or {}}).encode()
            mask = os.urandom(4)
            hdr = bytes([0x81]) + (bytes([0x80 | len(raw)]) if len(raw) < 126 else bytes([0x80 | 126]) + struct.pack(">H", len(raw)))
            s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(raw)))
            while True:
                m = recv()
                if m.get("id") == ids[0]:
                    return m.get("result", m.get("error"))

        send("Emulation.setDeviceMetricsOverride", {"width": WIDTH, "height": 800, "deviceScaleFactor": 1, "mobile": False})
        send("Page.enable")
        send("Page.navigate", {"url": URL})
        time.sleep(WAIT)
        result = send("Runtime.evaluate", {"expression": BOXES_JS, "returnByValue": True})
        return [tuple(b) for b in json.loads(result["result"]["value"])]
    finally:
        proc.kill()


def rdx_boxes():
    env = dict(os.environ, TRY_MEM="1G", TRY_TIMEOUT=str(int(60 + WAIT * 2)),
               TRY_EXTRA="-smp 2 -netdev user,id=n0 -device e1000,netdev=n0")
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "try.py"), "sleep 5000", "set BROWSER_BOXES=1", f"set BROWSER_WIDTH={WIDTH}", "set BROWSER_HEIGHT=800",
                          f"set BROWSER_WAIT={int(WAIT * 1000)}", f"browser --dump {RDX_URL}"],
                         capture_output=True, text=True, env=env, cwd=ROOT).stdout
    boxes = []
    for line in out.splitlines():
        m = re.match(r"\[box\] \d+ (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (\S+) (\S+)", line.strip())
        if m:
            g = m.groups()
            boxes.append((g[0], int(g[1]), int(g[2]), int(g[3]), int(g[4]), g[5], g[6]))
    return boxes


chrome, rdx = chrome_boxes(), rdx_boxes()
print(f"Chrome: {len(chrome)} elements, RDX: {len(rdx)} elements (width {WIDTH})")
matcher = difflib.SequenceMatcher(None, [b[0] for b in chrome], [b[0] for b in rdx], autojunk=False)
pairs = []
for block in matcher.get_matching_blocks():
    for k in range(block.size):
        pairs.append((chrome[block.a + k], rdx[block.b + k]))
bad = [(c, r) for c, r in pairs if any(abs(c[i] - r[i]) > TOL for i in range(1, 5))]
print(f"{len(pairs)} matched, {len(pairs) - len(bad)} within {TOL}px, {len(bad)} differ")
print(f"{'element':40} {'chrome x,y w x h':>22}   {'rdx x,y w x h':>22}")
for c, r in bad[:SHOW]:
    name = c[0] + ("#" + c[5] if c[5] != "-" else "") + ("." + c[6] if c[6] != "-" else "")
    print(f"{name[:40]:40} {c[1]:5},{c[2]:<5} {c[3]:4}x{c[4]:<4}   {r[1]:5},{r[2]:<5} {r[3]:4}x{r[4]:<4}")
