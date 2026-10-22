#!/usr/bin/env python3
# dashboard.py [port] : read-only text dashboard of the streaming pipeline, http://<mac>.local:8765
import json, os, re, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOTTLE = os.environ.get("BOTTLE", "SteamX64")
BOTTLE_PATH = os.environ.get("BOTTLE_PATH", os.path.expanduser("~/CXPBottles"))
DRIVE = BOTTLE if BOTTLE.startswith("/") else f"{BOTTLE_PATH}/{BOTTLE}"
DRIVE = f"{DRIVE}/drive_c"
MQL = f"{DRIVE}/macquestlink"
LOGS = f"{DRIVE}/Program Files (x86)/Steam/logs"
PROCS = {"hlvr": "hlvr.exe", "vrserver": "vrserver.exe", "vrcompositor": "vrcompositor.exe",
         "vrmonitor": "vrmonitor.exe", "steam": "steam.exe", "wineserver": "wineserver"}
stats = {}


def tail(path, size=65536):
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - size))
            return f.read().decode(errors="replace").splitlines()
    except OSError:
        return []


def last(lines, needle):
    return next((l for l in reversed(lines) if needle in l), "")


STALE_S = 15


def fresh(lines, needle, end=None):
    """Last line containing needle, or "" when its HH:MM:SS stamp is older than STALE_S or a later line contains end."""
    for i in range(len(lines) - 1, -1, -1):
        if end and end in lines[i]:
            return ""
        if needle in lines[i]:
            try:
                t = time.strptime(time.strftime("%Y-%m-%d ") + lines[i][:8], "%Y-%m-%d %H:%M:%S")
            except ValueError:
                return ""
            age = (time.time() - time.mktime(t)) % 86400
            return lines[i] if age < STALE_S else ""
    return ""


def fresh_file(path):
    try:
        return time.time() - os.path.getmtime(path) < STALE_S
    except OSError:
        return False


def osa(script):
    try:
        return subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=2).stdout.strip()
    except subprocess.TimeoutExpired:
        return "?"


def collect():
    s = {"time": time.strftime("%H:%M:%S")}
    m = re.search(r"([\d.]+) fps, avg ([\d.]+) ms encode, ([\d.]+) Mbit/s",
                  fresh(tail(f"{MQL}/encoder.log"), "frames,", "client disconnected"))
    if m:
        s["stream fps"], s["encode ms"], s["Mbit/s"] = m.groups()
    shim = tail(f"{MQL}/nvenc_shim.log")
    m = re.search(r"queued ([\d./]+) gpu ([\d./]+) copy\+helper ([\d./]+) total ([\d./]+)", fresh(shim, "timing avg/max"))
    if m:
        s["path avg/max ms"] = f"total {m[4]}  (queued {m[1]}, gpu {m[2]}, copy+encode {m[3]})"
    vrlink = tail(f"{LOGS}/driver_vrlink.txt", 262144)
    now = time.time()
    bad = [l for l in vrlink if "Bad link event" in l]
    recent = 0
    for l in bad:
        try:
            t = time.mktime(time.strptime(l[:24], "%a %b %d %Y %H:%M:%S"))
            recent += now - t < 60
        except ValueError:
            pass
    s["bad link /min"] = recent
    m = re.search(r"-> ([\d.]+) mbit/s, ping ([\d.]+) ms", bad[-1] if bad else "")
    if m:
        s["vrlink target / ping"] = f"{m[1]} Mbit/s / {m[2]} ms"
    ft = f"{MQL}/frametiming.log"
    m = re.search(r"app gpu pre/post/total [\d.]+/[\d.]+/([\d.]+) ms", last(tail(ft), "new frames") if fresh_file(ft) else "")
    if m:
        s["frame ms"] = m[1]
    ps = subprocess.run(["ps", "-Ao", "%cpu=,command="], capture_output=True, text=True).stdout.splitlines()
    for key, name in PROCS.items():
        cpu = sum(float(l.split(None, 1)[0]) for l in ps if name in l and "grep" not in l)
        s[f"cpu {key} %"] = f"{cpu:.0f}"
    m = re.search(r'"Device Utilization %"=(\d+)', subprocess.run(
        ["ioreg", "-r", "-d", "1", "-c", "IOAccelerator"], capture_output=True, text=True).stdout)
    s["gpu %"] = m[1] if m else "?"
    front = subprocess.run(["lsappinfo", "info", "-only", "name", subprocess.run(["lsappinfo", "front"], capture_output=True, text=True).stdout.strip()], capture_output=True, text=True).stdout
    m = re.search(r'"([^"]+)"', front)
    s["frontmost"] = m[1] if m else "?"
    s["screen locked"] = "IOConsoleLocked</key>\n\t<true/>" in subprocess.run(
        ["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, text=True).stdout
    return s


def num(v, i=0):
    m = re.findall(r"[\d.]+", str(v))
    return m[i] if len(m) > i else "?"


def panel(s):
    return "\r\n".join([
        f"fps    {s.get('stream fps', '?')}",
        f"Mbit   {num(s.get('Mbit/s'))}",
        f"frame  {s.get('frame ms', '?')} ms",
        f"enc    {s.get('encode ms', '?')} ms",
        f"path   {num(s.get('path avg/max ms'))} ms",
        f"gpuwt  {num(s.get('path avg/max ms'), 4)} ms",
        f"badlnk {s.get('bad link /min', '?')}/min",
        f"ping   {num(s.get('vrlink target / ping'), 1)} ms",
        f"alyx   {s.get('cpu hlvr %')}%",
        f"vrsrv  {s.get('cpu vrserver %')}%",
        f"wsrv   {s.get('cpu wineserver %')}%",
        f"gpu    {s.get('gpu %')}%",
        f"       {s.get('time')}",
    ])


def loop():
    global stats
    while True:
        try:
            stats = collect()
            with open(f"{MQL}/stats.txt.new", "w", newline="") as f:
                f.write(panel(stats))
            os.replace(f"{MQL}/stats.txt.new", f"{MQL}/stats.txt")
        except Exception as e:
            stats = {"error": repr(e)}
        time.sleep(1)


PAGE = """<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>macquestlink</title><style>body{font:15px ui-monospace,Menlo,monospace;background:#111;color:#ddd;margin:16px}
td{padding:2px 12px 2px 0;vertical-align:top}td:first-child{color:#888}.bad{color:#f66}.ok{color:#6d6}</style>
<table id=t></table><script>
async function tick(){try{const s=await (await fetch('/stats')).json();let h='';for(const[k,v]of Object.entries(s)){
let c='';if(k=='stream fps')c=+v>=68?'ok':'bad';if(k=='frontmost')c=v=='hlvr.exe'?'ok':'';if(k=='screen locked')c=v?'bad':'ok';
if(k=='bad link /min')c=v>5?'bad':'ok';if(k=='frame ms')c=+v<13.9?'ok':'bad';h+=`<tr><td>${k}</td><td class=${c}>${v}</td></tr>`}t.innerHTML=h}catch(e){}}
tick();setInterval(tick,1000)</script>"""


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body, kind = (json.dumps(stats), "application/json") if self.path == "/stats" else (PAGE, "text/html")
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


threading.Thread(target=loop, daemon=True).start()
ThreadingHTTPServer(("0.0.0.0", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), Handler).serve_forever()
