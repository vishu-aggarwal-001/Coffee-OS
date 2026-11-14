#!/usr/bin/env python3
"""
Machine simulator: runs the touchscreen and owner app on your laptop with no hardware.

    python3 simulator.py            then open  http://localhost:8080/          (touchscreen)
                                          and  http://localhost:8080/counter   (cashier screen, PIN 1234)
                                          and  http://localhost:8080/owner     (owner app, PIN 000000)

It serves the same files the controller serves (../firmware/CoffeeController/data)
and fakes the controller's API, so you can try every screen before building a machine.
"""
import json, os, sys, time, threading, copy
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "firmware", "CoffeeController", "data")
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
SPEED = 3.0                     # simulated drinks run 3x faster than real ones
PIN = "000000"

recipes = json.load(open(os.path.join(DATA, "recipes.json")))
settings = {"apSsid": "CoffeeMachine-SIM", "staSsid": "", "freeVend": False, "counterPin": "1234", "codeHours": 12, "rinseHour": 3, "lids": True, "bottled": True, "bottleSensors": False, "moving": False, "chiller": False, "paused": False, "cupMode": "turret",
            "tz": "EST5EDT,M3.2.0,M11.1.0"}
BOTTLE_PAIR_ML = 22700
inventory = {"water_ml": 2600, "beans_g": 1180, "canisters_g": [2600, 1400, 950, 210], "syrups_ml": [700, 640], "cups": {"10": 52, "12": 61, "16": 17}, "lids": 140}
stats = {"count": 0, "revenueCents": 0, "perDrink": {}, "days": {}}
for i, (c, r) in enumerate([(31, 7420), (44, 10960), (38, 9310), (52, 13100), (47, 11800), (29, 7050)]):
    stats["days"][time.strftime("%Y-%m-%d", time.localtime(time.time() - (6 - i) * 86400))] = {"count": c, "revenueCents": r}
stats["perDrink"] = {"french-vanilla": 61, "americano": 48, "latte": 37, "hot-chocolate": 33, "cappuccino": 22, "mocha": 18}
sales = ["time,drink,size,price_cents,payment,result"]
faults = []
order = {"phase": "idle"}
codes = []   # open counter codes
lock = threading.Lock()

STEP_LABEL = {"espresso": ["Grinding fresh beans", "Brewing espresso"], "mix": "Steaming it together", "water": "Pouring hot water",
              "syrup": "Adding flavor", "wait": "Settling"}

def water_status():
    ml = inventory.get("water_ml", 0); sensors = settings.get("bottled") and settings.get("bottleSensors")
    empty = bool(settings.get("bottled")) and not sensors and ml < 300
    w = {"bottled": bool(settings.get("bottled")), "sensors": bool(sensors), "ml": int(ml), "fullMl": BOTTLE_PAIR_ML,
         "empty": empty, "low": bool(settings.get("bottled")) and not empty and not sensors and ml < 3000}
    if sensors: w.update(bottle1=True, bottle2=True)
    return w

def today():
    return time.strftime("%Y-%m-%d")

def find(did):
    return next((d for d in recipes["drinks"] if d["id"] == did), None)

def blocked():
    if faults: return "; ".join(faults)
    if settings.get("paused"): return "Closed"
    return ""

def menu():
    why = blocked()
    out = {"brand": recipes.get("brand"), "currency": recipes.get("currency", "USD"), "sugarLevels": 3, "open": not why, "drinks": [],
           "payMode": "free" if settings["freeVend"] else "counter"}
    if why: out["reason"] = why
    for d in recipes["drinks"]:
        if not d.get("enabled", True): continue
        if not settings.get("chiller") and d.get("temp") == "cold": continue
        o = {k: d.get(k) for k in ("id", "name", "desc", "layers")}
        o["temp"] = d.get("temp", "hot"); o["sugar"] = d.get("sugar", True); o["prices"] = d["prices"]
        if "fill" in d: o["fill"] = d["fill"]
        o["available"] = {s: (True if inventory["cups"].get(s, 0) > 0 else "Out of this cup size") for s in d["prices"]}
        out["drinks"].append(o)
    return out

def run_order(d, size, price, test, code=""):
    labels = ["Dropping your cup"]
    for s in d["steps"]:
        l = STEP_LABEL.get(s["op"], "Working")
        if s["op"] == "water" and s.get("water") == "cold": l = "Pouring chilled water"
        if s["op"] == "mix" and s.get("water") == "cold": l = "Mixing it cold"
        labels += l if isinstance(l, list) else [l]
    labels.append("Almost done")
    total = (8 + 6 * len(labels)) / SPEED
    t0 = time.time()
    with lock: order.update(phase="making", progress=0.0, label=labels[0]); order.pop("message", None)
    while True:
        p = (time.time() - t0) / total
        if p >= 1: break
        with lock: order.update(progress=round(min(p, 0.97), 3), label=labels[min(int(p * len(labels)), len(labels) - 1)])
        time.sleep(0.15)
    inventory["cups"][size] = max(0, inventory["cups"][size] - 1)
    with lock:
        order.update(phase="ready", progress=1.0, label="Ready")
        stats["count"] += 1; stats["revenueCents"] += 0 if test else price
        day = stats["days"].setdefault(today(), {"count": 0, "revenueCents": 0}); day["count"] += 1; day["revenueCents"] += 0 if test else price
        stats["perDrink"][d["id"]] = stats["perDrink"].get(d["id"], 0) + 1
        sales.append(f"{time.strftime('%Y-%m-%d %H:%M:%S')},{d['id']},{size},{price},{'counter' if code else 'test'},ok")
        codes[:] = [c for c in codes if c["code"] != code]
    time.sleep(6)
    with lock: order.clear(); order["phase"] = "idle"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Admin-Pin")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS")
        self.send_header("Cache-Control", "no-store")
    def json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code); self.cors(); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        return json.loads(self.rfile.read(n) or b"{}") if n else {}
    def admin(self):
        if self.headers.get("X-Admin-Pin") == PIN: return True
        self.json({"ok": False, "error": "Wrong PIN"}, 401); return False
    def do_OPTIONS(self):
        self.send_response(204); self.cors(); self.end_headers()
    def do_GET(self):
        p = urlparse(self.path).path
        if p == "/api/menu": return self.json(menu())
        if p == "/api/status":
            with lock: o = dict(order)
            o.setdefault("phase", "idle"); return self.json(o)
        if p == "/api/counter/list":
            if self.headers.get("X-Counter-Pin") not in (settings["counterPin"], PIN): return self.json({"ok": False, "error": "Wrong PIN"}, 401)
            return self.json({"menu": menu(), "codes": codes, "freeMode": settings["freeVend"], "water": water_status()})
        if p == "/api/admin/summary":
            if not self.admin(): return
            with lock: st = dict(order)
            return self.json({"status": st, "temps": {"espresso": 99.4, "boiler": 94.8, "cold": 5.6, "cabinet": 27.1},
                              "faults": "; ".join(faults), "faultBits": 1 if faults else 0, "selling": not blocked(), "blockedReason": blocked(),
                              "inventory": inventory, "stats": stats, "testMode": settings["freeVend"], "firmware": "1.2.0-sim",
                              "uptimeMin": 1442, "wifi": {"ap": settings["apSsid"], "store": settings["staSsid"], "storeConnected": bool(settings["staSsid"])},
                              "settings": {k: settings[k] for k in ("rinseHour", "lids", "bottled", "bottleSensors", "codeHours", "moving", "chiller", "paused", "cupMode")}, "water": water_status(),
                              "io": {"relays": True, "inputs": True, "addons": True}, "openCodes": codes, "time": time.strftime("%Y-%m-%d %H:%M:%S")})
        if p == "/api/admin/recipes":
            if not self.admin(): return
            return self.json(recipes)
        if p == "/api/admin/sales.csv":
            if not self.admin(): return
            b = ("\n".join(sales) + "\n").encode()
            self.send_response(200); self.cors(); self.send_header("Content-Type", "text/csv"); self.end_headers(); self.wfile.write(b); return
        if p in ("/", ""): p = "/index.html"
        if p in ("/owner", "/owner/"): p = "/owner.html"
        if p in ("/counter", "/counter/"): p = "/counter.html"
        f = os.path.normpath(os.path.join(DATA, p.lstrip("/")))
        if f.startswith(os.path.normpath(DATA)) and os.path.isfile(f):
            ct = "text/html" if f.endswith(".html") else "application/json" if f.endswith((".json", ".webmanifest")) else "image/svg+xml" if f.endswith(".svg") else "text/plain"
            b = open(f, "rb").read()
            self.send_response(200); self.send_header("Content-Type", ct); self.send_header("Cache-Control", "no-cache"); self.end_headers(); self.wfile.write(b); return
        self.json({"ok": False, "error": "Not found"}, 404)
    def do_PUT(self): self.do_POST()
    def do_POST(self):
        global recipes
        p = urlparse(self.path).path
        b = self.body()
        if p == "/api/order":
            if order.get("phase", "idle") != "idle": return self.json({"ok": False, "error": "The machine is busy"}, 400)
            if blocked(): return self.json({"ok": False, "error": blocked()}, 400)
            d = find(b.get("drink", "")); size = str(b.get("size", "12"))
            if not d or size not in d["prices"]: return self.json({"ok": False, "error": "That drink is not available"}, 400)
            price = round(d["prices"][size] * 100); code = str(b.get("code", ""))
            if not settings["freeVend"]:
                v = next((c for c in codes if c["code"] == code), None)
                if not v: return self.json({"ok": False, "error": "That code didn't work. Check it with the cashier."}, 400)
                if v["drink"] != d["id"] or v["size"] != size: return self.json({"ok": False, "error": "That code is for a different drink or size"}, 400)
            else: code = ""
            with lock: order.clear(); order.update(phase="making", drink=d["name"], size=size, price=price / 100, progress=0)
            threading.Thread(target=run_order, args=(d, size, price if code else 0, not code, code), daemon=True).start()
            return self.json({"ok": True})
        if p == "/api/redeem":
            v = next((c for c in codes if c["code"] == str(b.get("code", ""))), None)
            if not v: return self.json({"ok": False, "error": "That code didn't work. Check it with the cashier."}, 400)
            return self.json({"ok": True, "drink": v["drink"], "size": v["size"]})
        if p.startswith("/api/counter/"):
            if self.headers.get("X-Counter-Pin") not in (settings["counterPin"], PIN): return self.json({"ok": False, "error": "Wrong PIN"}, 401)
            if p == "/api/counter/create":
                d = find(b.get("drink", "")); size = str(b.get("size", "12"))
                if not d or size not in d["prices"]: return self.json({"ok": False, "error": "That drink is not on the menu"}, 400)
                import random
                code = str(random.randint(1000, 9999))
                codes.append({"code": code, "drink": d["id"], "size": size, "name": d["name"], "price": d["prices"][size], "at": int(time.time())})
                return self.json({"ok": True, "code": code})
            if p == "/api/counter/bottles":
                inventory["water_ml"] = BOTTLE_PAIR_ML
                return self.json({"ok": True, "water": water_status()})
            if p == "/api/counter/cancel":
                codes[:] = [c for c in codes if c["code"] != str(b.get("code", ""))]
                return self.json({"ok": True})
        if p == "/api/cancel":
            with lock:
                if order.get("phase") == "paying": order.clear(); order["phase"] = "idle"
            return self.json({"ok": True})
        if p == "/api/tablet": return self.json({"ok": True})
        if not p.startswith("/api/admin/"): return self.json({"ok": False, "error": "Not found"}, 404)
        if not self.admin(): return
        if p == "/api/admin/recipes":
            if not b.get("drinks"): return self.json({"ok": False, "error": "Add at least one drink."}, 400)
            for d in b["drinks"]:
                if not d.get("steps"): return self.json({"ok": False, "error": f"Drink {d.get('name')} has no steps."}, 400)
            b.setdefault("calibration", recipes.get("calibration")); recipes = b
            return self.json({"ok": True})
        if p == "/api/admin/inventory":
            for k in ("water_ml", "beans_g", "canisters_g", "syrups_ml", "cups", "lids"):
                if k in b: inventory[k] = b[k]
            return self.json({"ok": True})
        if p == "/api/admin/settings":
            for k, v in b.items():
                if k == "cupMode" and v not in ("turret", "droppers"): return self.json({"ok": False, "error": "cupMode must be turret or droppers"}, 400)
                if k in ("adminPin", "counterPin"):
                    if len(str(v)) < 4: return self.json({"ok": False, "error": "PIN must be at least 4 digits"}, 400)
                    continue   # simulator keeps the default PIN
                settings[k] = v
            return self.json({"ok": True, "restarting": any(k.startswith(("ap", "sta")) for k in b)})
        if p == "/api/admin/action":
            a = b.get("action")
            if a == "testVend":
                d = find(b.get("drink", "")); size = str(b.get("size", "12"))
                if not d: return self.json({"ok": False, "error": "That drink is not available"}, 400)
                if order.get("phase", "idle") != "idle": return self.json({"ok": False, "error": "The machine is busy"}, 400)
                with lock: order.clear(); order.update(phase="making", drink=d["name"], size=size, price=0)
                threading.Thread(target=run_order, args=(d, size, 0, True), daemon=True).start()
            elif a == "clearFaults": faults.clear()
            elif a == "bottlesSwapped": inventory["water_ml"] = BOTTLE_PAIR_ML
            elif a == "moveMode":
                settings["moving"] = bool(b.get("on", True))
                if settings["moving"]: faults[:] = ["Out of service: getting ready to move"]
                else: faults.clear()
            elif a == "simulateFault": faults.append("Wastewater bucket is full")
            elif a == "resetStats": stats.update(count=0, revenueCents=0, perDrink={}, days={})
            elif a == "rinse":
                if order.get("phase", "idle") != "idle": return self.json({"ok": False, "error": "The machine is busy"}, 400)
                with lock: order.clear(); order.update(phase="making", drink="Rinse", size="", price=0, progress=0, label="Rinsing")
                def rinse():
                    time.sleep(4)
                    with lock: order.clear(); order["phase"] = "idle"
                threading.Thread(target=rinse, daemon=True).start()
            elif a == "cancel":
                with lock: order.clear(); order["phase"] = "idle"
            elif a in ("prime", "unlockDoor", "restart", "dispense"): pass
            else: return self.json({"ok": False, "error": "Unknown action"}, 400)
            return self.json({"ok": True})
        self.json({"ok": False, "error": "Not found"}, 404)

if __name__ == "__main__":
    print(f"Simulator running: touchscreen http://localhost:{PORT}/   counter http://localhost:{PORT}/counter (PIN 1234)   owner http://localhost:{PORT}/owner (PIN {PIN})")
    ThreadingHTTPServer(("0.0.0.0", PORT), H).serve_forever()
