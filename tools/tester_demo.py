#!/usr/bin/env python3
"""Run the web UI on your computer without any hardware.

Serves firmware/ui/index.html and answers the API with made-up data in the same
shape as the real device. Good for trying the UI and for screenshots.

    python3 tools/tester_demo.py            # http://localhost:8080/
    python3 tools/tester_demo.py --ap       # pretend the device is in setup mode
"""
import argparse, json, time, pathlib
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

UI = pathlib.Path(__file__).resolve().parent.parent / "firmware" / "ui"
NOW = int(time.time())

DEMO = [  # (lines, ms) - formats as printed by ComponentTester 1.57m
    (["BJT PNP", "123=CBE", "hFE 298 c", "Vbe 626mV", "Iceo 19nA", "Cbe 9pF"], 4893),
    (["BJT NPN", "123=EBC", "hFE 243 e", "Vbe 679mV", "Cbe 14pF"], 4880),
    (["1->|-3", "Vf 686mV", "C 1pF"], 4131),
    (["MOSFET", "N-ch enh.", "123=GDS", "Vth 2.1V", "Cgs 1.2nF", "Rds 0.05R", "Vf 0.71V"], 5210),
    (["1-[]-2-[]-3", "4.70k 10.0k"], 4602),
    (["1-||-3", "100.2uF", "ESR 0.12R", "Vloss 1.2%"], 6911),
    (["->|- 4"], 4966),
    (["No component", "found!"], 4886),
]
results = [{"id": i + 1, "ts": NOW - (len(DEMO) - i) * 95, "ms": ms, "type": l[0], "lines": l, "values": {}}
           for i, (l, ms) in enumerate(DEMO)]
SCREEN = ["Select:", "PWM", "IR detector", "Opto Coupler", "Test", "*Adjustment", "Save", "Load", "Show Values", "Exit"]
AP = False

def status():
    return {"fw": "1.0.0", "uptime_s": 4210, "mode": "ap" if AP else "sta", "ap_active": AP,
            "ssid": "" if AP else "HomeWiFi", "ip": "192.168.4.1" if AP else "192.168.1.50", "rssi": -57,
            "mac": "AA:BB:CC:DD:EE:FF", "screen_version": 812, "clears": 40, "results_stored": len(results),
            "fs_ok": True, "fs_used_kb": 12, "fs_total_kb": 1408, "time": NOW}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send(self, code, body, ctype="application/json"):
        b = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", len(b))
        self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        if u.path == "/": return self.send(200, (UI / "index.html").read_bytes(), "text/html; charset=utf-8")
        if u.path == "/openapi.json": return self.send(200, (UI / "openapi.json").read_bytes())
        if u.path == "/status": return self.send(200, status())
        if u.path == "/results":
            n = int(q.get("limit", ["50"])[0]); return self.send(200, {"total": len(results), "results": results[-n:]})
        if u.path == "/result":
            r = [x for x in results if x["id"] == int(q.get("id", ["0"])[0])]
            return self.send(200, r[0]) if r else self.send(404, {"error": "no such result"})
        if u.path == "/last": return self.send(200, results[-2])
        if u.path == "/screen": return self.send(200, {"rows": SCREEN, "version": 812, "stable_ms": 1200, "clears": 40})
        if u.path == "/history": return self.send(200, {"screens": [{"age_s": 2, "text": "\n".join(SCREEN) + "\n"}]})
        if u.path == "/wifi/scan":
            time.sleep(0.6)
            return self.send(200, {"networks": [{"ssid": "HomeWiFi", "rssi": -52, "open": False},
                                                {"ssid": "Neighbour-5G", "rssi": -78, "open": False},
                                                {"ssid": "Cafe-Guest", "rssi": -84, "open": True}]})
        self.send(404, {"error": "not found"})
    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/measure":
            time.sleep(1.0); r = dict(results[0]); r.update(ok=True, mode="reset"); return self.send(200, r)
        if u.path in ("/button", "/reset"): return self.send(200, {"rows": SCREEN, "version": 813, "stable_ms": 0, "clears": 41})
        if u.path == "/results/clear": return self.send(200, {"cleared": True})
        if u.path == "/wifi": return self.send(200, {"saved": True, "restart_in_ms": 1000})
        if u.path == "/update": return self.send(200, {"updated": True})
        self.send(404, {"error": "not found"})

if __name__ == "__main__":
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=8080); ap.add_argument("--ap", action="store_true")
    a = ap.parse_args(); AP = a.ap
    print(f"Demo UI on http://localhost:{a.port}/  (setup mode: {AP})")
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()
