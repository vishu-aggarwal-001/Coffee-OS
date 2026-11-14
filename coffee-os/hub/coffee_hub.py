#!/usr/bin/env python3
"""
Coffee Hub - the small server that runs on the Raspberry Pi behind the touchscreen.

The KinCony controller (ESP32) still makes every drink and owns every safety
decision. The hub sits between the screens and the controller and adds what a
tiny ESP32 can't do well:

  * Serves the touchscreen, counter screen and owner app, so the screen comes up
    even while the controller is rebooting.
  * Passes every /api/... request to the controller one at a time (the ESP32 web
    server handles one client at a time) and shares quick answers between
    screens, so the controller is not flooded.
  * Copies the sales log off the controller into a database that never fills up.
  * Watches the controller once a minute and sends a phone alert (ntfy) when a
    fault appears, the water runs low, or the machine goes offline.
  * Hub page at /hub with sales history, events and a fleet view of your other
    machines (over Tailscale or any network the hubs can reach).
  * Control page at /control (v2): open and close sales, rinse, prime, refill
    counts, test drinks, opening hours, all with one PIN. The same page opens on
    the touchscreen as a service menu (hold the top-left corner for 3 seconds).
  * Low-stock alerts and an end-of-day sales message on your phone.

Every button on the control page becomes a normal request to the controller,
which still checks it against its own safety rules. The hub never switches a
heater, pump or valve itself.

Python 3.9+ standard library only. No pip installs.

    python3 coffee_hub.py --config /etc/coffee-hub/config.json
    python3 coffee_hub.py --machine http://127.0.0.1:8080 --port 8000 --data ./data   (try it with the simulator)
"""
import argparse, csv, hmac, io, json, logging, os, signal, socket, sqlite3, sys, threading, time
import urllib.error, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VERSION = "2.0.0"
HERE = os.path.dirname(os.path.abspath(__file__))
log = logging.getLogger("coffee-hub")

DEFAULTS = {
    "machine_name": "coffee-01",          # shown on the hub page and in alerts
    "machine_url": "http://192.168.4.1",  # the controller (its own Wi-Fi), or http://coffee.local on store Wi-Fi
    "listen_host": "0.0.0.0",
    "listen_port": 8000,
    "data_dir": "/var/lib/coffee-hub",
    "ui_dir": os.path.join(HERE, "..", "ui"),
    "owner_pin": "",          # the machine's owner PIN; lets the hub copy sales and watch for faults
    "hub_pin": "",            # PIN for the /hub page; empty = same as owner_pin
    "sync_minutes": 5,        # how often to copy the sales log
    "watch_seconds": 60,      # how often to check the machine for faults
    "ntfy_topic": "",         # e.g. "vishu-coffee-8f3k2"; install the ntfy app and subscribe to it
    "ntfy_server": "https://ntfy.sh",
    "peers": [],              # other hubs for the fleet view, e.g. ["http://coffee-02:8000"]
    "ui_from_controller": False,  # True = show the screens stored on the controller instead of the copy in CoffeeOS
    # v2 control page
    "capacity": {"beans_g": 1200, "canister_g": 2000, "syrup_ml": 1000, "cups": 40},   # what "Refilled" sets each count to
    "low_stock": {"beans_g": 150, "canister_g": 150, "syrup_ml": 100, "cups": 10},     # phone alert below these
    "canister_names": ["Sugar", "Creamer", "Chocolate", "French vanilla"],
    "syrup_names": ["Vanilla", "Caramel"],
    "daily_report": "20:30",      # phone message with the day's sales at this time; "" = off
    "service_corner": True,       # hold the touchscreen's top-left corner 3 s to open the service menu
}

DAYS = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
DEFAULT_SCHEDULE = {"enabled": False, "days": {d: {"on": True, "open": "07:00", "close": "19:00"} for d in DAYS}}

# Buttons on the control page. Each maps to one controller request.
# (label for the log, controller path, body builder)
CONTROL_ACTIONS = {
    "open":          ("Opened sales",              "/api/admin/settings", lambda a: {"paused": False}),
    "close":         ("Closed sales",              "/api/admin/settings", lambda a: {"paused": True}),
    "rinse":         ("Started a rinse",           "/api/admin/action",   lambda a: {"action": "rinse"}),
    "prime":         ("Primed the water",          "/api/admin/action",   lambda a: {"action": "prime"}),
    "clearFaults":   ("Cleared faults",            "/api/admin/action",   lambda a: {"action": "clearFaults"}),
    "bottlesSwapped":("Bottles swapped",           "/api/admin/action",   lambda a: {"action": "bottlesSwapped"}),
    "unlockDoor":    ("Unlocked the door",         "/api/admin/action",   lambda a: {"action": "unlockDoor"}),
    "cancel":        ("Stopped the current drink", "/api/admin/action",   lambda a: {"action": "cancel"}),
    "restart":       ("Restarted the controller",  "/api/admin/action",   lambda a: {"action": "restart"}),
    "testVend":      ("Made a test drink",         "/api/admin/action",
                      lambda a: {"action": "testVend", "drink": str(a.get("drink", "")), "size": str(a.get("size", "12"))}),
    "moveOn":        ("Get ready to move",         "/api/admin/action",   lambda a: {"action": "moveMode", "on": True}),
    "moveOff":       ("Back in service",           "/api/admin/action",   lambda a: {"action": "moveMode", "on": False}),
    "freeOn":        ("Free mode on",              "/api/admin/settings", lambda a: {"freeVend": True}),
    "freeOff":       ("Free mode off",             "/api/admin/settings", lambda a: {"freeVend": False}),
    "cupsTurret":    ("Cup mode: turret",          "/api/admin/settings", lambda a: {"cupMode": "turret"}),
    "cupsDroppers":  ("Cup mode: printed droppers", "/api/admin/settings", lambda a: {"cupMode": "droppers"}),
}


def hhmm(t):
    h, m = str(t).split(":")
    h, m = int(h), int(m)
    if not (0 <= h <= 24 and 0 <= m < 60):
        raise ValueError(t)
    return h * 60 + m


def scheduled_open(sched, now=None):
    """True/False when the opening hours say open/closed right now, None when hours are off."""
    if not sched.get("enabled"):
        return None
    lt = time.localtime(now or time.time())
    day = (sched.get("days") or {}).get(DAYS[lt.tm_wday]) or {}
    if not day.get("on"):
        return False
    minute = lt.tm_hour * 60 + lt.tm_min
    a, b = hhmm(day.get("open", "00:00")), hhmm(day.get("close", "24:00"))
    return a <= minute < b if a <= b else (minute >= a or minute < b)     # handles hours past midnight

UI_FILES = ["index.html", "counter.html", "owner.html", "owner.webmanifest", "icon.svg"]
CTYPES = {".html": "text/html; charset=utf-8", ".json": "application/json", ".webmanifest": "application/manifest+json",
          ".svg": "image/svg+xml", ".png": "image/png", ".js": "text/javascript", ".css": "text/css", ".csv": "text/csv"}
CACHE_TTL = {"/api/status": 0.4, "/api/menu": 2.0}   # shared answers for the public screens


def load_config(path, overrides):
    cfg = json.loads(json.dumps(DEFAULTS))
    if path and os.path.exists(path):
        with open(path) as f:
            for k, v in json.load(f).items():
                if isinstance(cfg.get(k), dict) and isinstance(v, dict):
                    cfg[k].update(v)              # a partial "capacity" or "low_stock" keeps the other defaults
                else:
                    cfg[k] = v
    cfg.update({k: v for k, v in overrides.items() if v is not None})
    cfg["machine_url"] = cfg["machine_url"].rstrip("/")
    if not cfg["hub_pin"]:
        cfg["hub_pin"] = cfg["owner_pin"]
    return cfg


# --------------------------------------------------------------------------- controller link
class MachineDown(Exception):
    pass


class Machine:
    """Talks to the ESP32. One request at a time, short timeouts."""

    def __init__(self, url):
        self.url = url
        self.lock = threading.Lock()
        self.cache = {}
        self.cache_lock = threading.Lock()
        self.last_ok = 0.0
        self.last_error = ""

    def request(self, method, path, body=None, headers=None, timeout=5.0):
        req = urllib.request.Request(self.url + path, data=body, method=method, headers=headers or {})
        with self.lock:
            try:
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    status, ctype, data = r.status, r.headers.get("Content-Type", "application/octet-stream"), r.read()
            except urllib.error.HTTPError as e:              # the controller answered, just not with 200
                status, ctype, data = e.code, e.headers.get("Content-Type", "application/json"), e.read()
            except (urllib.error.URLError, socket.timeout, ConnectionError, OSError) as e:
                self.last_error = str(getattr(e, "reason", e))
                raise MachineDown(self.last_error)
        self.last_ok = time.time()
        self.last_error = ""
        return status, ctype, data

    def cached_get(self, path):
        ttl = CACHE_TTL[path]
        with self.cache_lock:
            hit = self.cache.get(path)
            if hit and time.time() - hit[0] < ttl:
                return hit[1]
        res = self.request("GET", path, timeout=3.0)
        if res[0] == 200:
            with self.cache_lock:
                self.cache[path] = (time.time(), res)
        return res

    def forget_cache(self):
        with self.cache_lock:
            self.cache.clear()

    def online(self):
        return time.time() - self.last_ok < 90

    def json(self, path, pin=None, timeout=5.0):
        h = {"X-Admin-Pin": pin} if pin else {}
        status, _, data = self.request("GET", path, headers=h, timeout=timeout)
        return status, data


# --------------------------------------------------------------------------- database
class Store:
    def __init__(self, data_dir):
        os.makedirs(data_dir, exist_ok=True)
        self.path = os.path.join(data_dir, "hub.sqlite3")
        self.lock = threading.Lock()
        self.db = sqlite3.connect(self.path, check_same_thread=False)
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")     # the machine gets unplugged without warning
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS sales (
              id INTEGER PRIMARY KEY, time TEXT, drink TEXT, size TEXT, price_cents INTEGER,
              payment TEXT, result TEXT, occ INTEGER, seen_at INTEGER,
              UNIQUE(time, drink, size, price_cents, payment, result, occ));
            CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY, at INTEGER, kind TEXT, message TEXT);
            CREATE TABLE IF NOT EXISTS readings (at INTEGER PRIMARY KEY, espresso REAL, boiler REAL, cold REAL,
              cabinet REAL, water_ml INTEGER, faults TEXT, selling INTEGER);
            CREATE INDEX IF NOT EXISTS sales_seen ON sales(seen_at);
        """)
        self.db.commit()

    def q(self, sql, args=()):
        with self.lock:
            return self.db.execute(sql, args).fetchall()

    def x(self, sql, args=()):
        with self.lock:
            cur = self.db.execute(sql, args)
            self.db.commit()
            return cur.rowcount

    def add_sales(self, rows):
        now, added = int(time.time()), 0
        with self.lock:
            for r in rows:
                cur = self.db.execute("INSERT OR IGNORE INTO sales(time,drink,size,price_cents,payment,result,occ,seen_at) "
                                      "VALUES(?,?,?,?,?,?,?,?)", r + (now,))
                added += cur.rowcount
            self.db.commit()
        return added

    def event(self, kind, message):
        log.info("%s: %s", kind, message)
        self.x("INSERT INTO events(at,kind,message) VALUES(?,?,?)", (int(time.time()), kind, message))

    def checkpoint(self):
        with self.lock:
            self.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        os.sync()


def parse_sales_csv(text):
    """Turn the controller's sales.csv into rows; 'occ' numbers identical lines so repeats are kept."""
    seen, rows = {}, []
    for rec in csv.reader(io.StringIO(text)):
        if len(rec) < 6 or rec[0] == "time":
            continue
        try:
            price = int(rec[3])
        except ValueError:
            continue
        key = (rec[0], rec[1], rec[2], price, rec[4], rec[5])
        seen[key] = seen.get(key, 0) + 1
        rows.append(key + (seen[key],))
    return rows


# --------------------------------------------------------------------------- the hub
class Hub:
    def __init__(self, cfg, config_path=None):
        self.cfg = cfg
        self.config_path = config_path
        self.started = time.time()
        self.m = Machine(cfg["machine_url"])
        self.store = Store(cfg["data_dir"])
        self.ui_cache = os.path.join(cfg["data_dir"], "ui-cache")
        os.makedirs(self.ui_cache, exist_ok=True)
        self.summary = None          # last /api/admin/summary from the controller
        self.summary_at = 0.0
        self.pin_rejected = False    # stop using a wrong owner PIN, or we'd lock the real owner out
        self.state = {"online": None, "faults": "", "water": "", "moving": False}
        self.offline_since = None
        self.stop = threading.Event()
        self.bad_logins = {}         # ip -> (count, until)
        self.sched_path = os.path.join(cfg["data_dir"], "schedule.json")
        self.schedule = self.load_schedule()
        self.sched_last = None       # what the hours last asked for; only changes are sent, so a manual Open/Close sticks
        self.low_alerted = set()     # stock items already alerted, until refilled
        self.report_sent = ""        # date of the last end-of-day message
        self.control_lock = threading.Lock()

    # ---- config reload (sudo systemctl reload coffee-hub)
    def reload(self):
        if not self.config_path:
            return
        try:
            new = load_config(self.config_path, {})
        except Exception as e:
            log.error("config reload failed: %s", e)
            return
        self.cfg.update(new)
        self.m.url = self.cfg["machine_url"]
        self.pin_rejected = False
        log.info("config reloaded")

    # ---- alerts
    def notify(self, title, message, priority="default"):
        topic = self.cfg.get("ntfy_topic")
        if not topic:
            return

        def send():
            try:
                req = urllib.request.Request(f'{self.cfg["ntfy_server"].rstrip("/")}/{topic}', data=message.encode(), method="POST",
                                             headers={"Title": f'{self.cfg["machine_name"]}: {title}', "Priority": priority, "Tags": "coffee"})
                urllib.request.urlopen(req, timeout=10).read()
            except Exception as e:      # alerts are best effort; the store has no internet sometimes
                log.warning("alert not sent: %s", e)
        threading.Thread(target=send, daemon=True).start()

    # ---- background: watch the controller
    def watch_once(self):
        pin = self.cfg.get("owner_pin")
        try:
            if pin and not self.pin_rejected:
                status, data = self.m.json("/api/admin/summary", pin)
                if status == 401:
                    self.pin_rejected = True
                    self.store.event("config", "The controller rejected the hub's owner PIN. Put the new PIN in "
                                     "/etc/coffee-hub/config.json, then run: sudo systemctl reload coffee-hub")
                    self.notify("Hub needs the new owner PIN", "Sales copying and fault alerts are paused.", "high")
                elif status == 200:
                    self.summary, self.summary_at = json.loads(data), time.time()
                    self.after_summary(self.summary)
            else:
                self.m.cached_get("/api/menu")       # at least know whether it's alive
            self.set_online(True)
        except MachineDown:
            self.set_online(False)
        except Exception as e:
            log.exception("watch failed: %s", e)

    def set_online(self, up):
        self.state["online"] = up
        if up:
            if self.state.get("offline_alerted"):
                self.store.event("online", "Controller is back online")
                self.notify("Back online", "The controller is answering again.")
            self.state["offline_alerted"] = False
            self.offline_since = None
            return
        if self.offline_since is None:
            self.offline_since = time.time()
        if time.time() - self.offline_since > 120 and not self.state.get("offline_alerted"):
            self.state["offline_alerted"] = True
            self.store.event("offline", f"Controller not answering: {self.m.last_error}")
            self.notify("Machine offline", "The touchscreen can't reach the controller. Check power and the machine's Wi-Fi.", "high")

    def after_summary(self, s):
        faults = s.get("faults") or ""
        if faults != self.state["faults"]:
            if faults:
                self.store.event("fault", faults)
                self.notify("Fault - sales stopped", faults, "high")
            elif self.state["faults"]:
                self.store.event("cleared", "Faults cleared, selling again")
                self.notify("Selling again", "Faults cleared.")
            self.state["faults"] = faults
        w = s.get("water") or {}
        water = "empty" if w.get("empty") else "low" if w.get("low") else ""
        if water != self.state["water"]:
            if water:
                self.store.event("water", f"Water {water}")
                self.notify(f"Water {water}", "Ask the station to put in 2 full bottles and press Bottles swapped.",
                            "high" if water == "empty" else "default")
            self.state["water"] = water
        self.check_stock(s.get("inventory") or {})
        moving = bool((s.get("settings") or {}).get("moving"))
        if moving and not self.state["moving"]:
            self.store.event("moving", "Get ready to move: data saved to disk")
            self.store.checkpoint()                          # safe to unplug
        self.state["moving"] = moving
        now = int(time.time())
        last = self.store.q("SELECT MAX(at) FROM readings")[0][0] or 0
        if now - last >= 600:                                # one reading every 10 minutes for history
            t = s.get("temps") or {}
            self.store.x("INSERT OR REPLACE INTO readings VALUES(?,?,?,?,?,?,?,?)",
                         (now, t.get("espresso"), t.get("boiler"), t.get("cold"), t.get("cabinet"),
                          (s.get("inventory") or {}).get("water_ml"), faults, 1 if s.get("selling") else 0))
            self.store.x("DELETE FROM readings WHERE at < ?", (now - 90 * 86400,))

    # ---- v2: stock alerts
    def low_items(self, inv):
        lim, out = self.cfg["low_stock"], []
        if inv.get("beans_g") is not None and inv["beans_g"] < lim["beans_g"]:
            out.append(("beans", f'Beans {int(inv["beans_g"])} g'))
        for i, g in enumerate(inv.get("canisters_g") or []):
            if g is not None and g < lim["canister_g"]:
                name = (self.cfg["canister_names"] + [""] * 8)[i] or f"Canister {i + 1}"
                out.append((f"can{i}", f"{name} {int(g)} g"))
        for i, ml in enumerate(inv.get("syrups_ml") or []):
            if ml is not None and ml < lim["syrup_ml"]:
                name = (self.cfg["syrup_names"] + [""] * 4)[i] or f"Syrup {i + 1}"
                out.append((f"syr{i}", f"{name} syrup {int(ml)} ml"))
        for size, n in sorted((inv.get("cups") or {}).items()):
            if n is not None and n < lim["cups"]:
                out.append((f"cups{size}", f"{size} oz cups: {int(n)} left"))
        return out

    def check_stock(self, inv):
        low = self.low_items(inv)
        keys = {k for k, _ in low}
        new = [text for k, text in low if k not in self.low_alerted]
        if new:
            self.store.event("stock", "Running low: " + ", ".join(new))
            self.notify("Running low", ", ".join(new) + ". Refill, then tap Refilled on the control page.")
        self.low_alerted = keys          # refilled items drop out, so they can alert again later

    # ---- v2: opening hours
    def load_schedule(self):
        try:
            with open(self.sched_path) as f:
                sch = json.load(f)
            days = {d: dict(DEFAULT_SCHEDULE["days"][d], **(sch.get("days") or {}).get(d, {})) for d in DAYS}
            return {"enabled": bool(sch.get("enabled")), "days": days}
        except (OSError, ValueError):
            return json.loads(json.dumps(DEFAULT_SCHEDULE))

    def save_schedule(self, sch):
        clean = {"enabled": bool(sch.get("enabled")), "days": {}}
        for d in DAYS:
            day = (sch.get("days") or {}).get(d) or {}
            o, c = str(day.get("open", "07:00")), str(day.get("close", "19:00"))
            hhmm(o), hhmm(c)                              # raises ValueError on a bad time
            clean["days"][d] = {"on": bool(day.get("on", True)), "open": o, "close": c}
        tmp = self.sched_path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(clean, f, indent=1)
        os.replace(tmp, self.sched_path)
        self.schedule = clean
        self.sched_last = None                            # apply the new hours on the next tick
        return clean

    def schedule_tick(self):
        want = scheduled_open(self.schedule)
        if want is None or want == self.sched_last:
            return
        ok, msg = self.control("open" if want else "close", {}, "opening hours")
        if ok:
            self.sched_last = want

    # ---- v2: end-of-day message
    def report_tick(self):
        at = self.cfg.get("daily_report") or ""
        today = time.strftime("%Y-%m-%d")
        if not at or self.report_sent == today:
            return
        lt = time.localtime()
        try:
            if lt.tm_hour * 60 + lt.tm_min < hhmm(at):
                return
        except ValueError:
            return
        self.report_sent = today
        t = self.today()
        low = self.low_items((self.summary or {}).get("inventory") or {})
        msg = f'{t["count"]} drinks, ${t["cents"] / 100:.2f} today.'
        if low:
            msg += " Low: " + ", ".join(x for _, x in low) + "."
        if self.state["faults"]:
            msg += " Fault: " + self.state["faults"]
        self.notify("Today", msg, "low")

    # ---- v2: control page actions
    def control(self, action, args, who):
        """Run one control-page button. Returns (ok, message)."""
        pin = self.cfg.get("owner_pin")
        if not pin:
            return False, "Put the owner PIN in /etc/coffee-hub/config.json first"
        if self.pin_rejected:
            return False, "The controller rejected the hub's owner PIN. Update config.json and reload the hub."
        if action == "refill":
            return self.refill(args, who)
        if action not in CONTROL_ACTIONS:
            return False, "Unknown button"
        label, path, build = CONTROL_ACTIONS[action]
        body = json.dumps(build(args)).encode()
        with self.control_lock:
            try:
                status, _, data = self.m.request("POST", path, body, {"Content-Type": "application/json", "X-Admin-Pin": pin}, timeout=10)
            except MachineDown:
                return False, "The controller is not answering"
        self.m.forget_cache()
        try:
            res = json.loads(data or b"{}")
        except ValueError:
            res = {}
        if status == 401:
            self.pin_rejected = True
            return False, "The controller rejected the hub's owner PIN"
        if status != 200 or res.get("ok") is False:
            return False, res.get("error") or f"Controller said HTTP {status}"
        detail = f' ({args.get("drink")} {args.get("size")} oz)' if action == "testVend" else ""
        self.store.event("control", f"{label}{detail} - {who}")
        self.summary_at = 0                                # fetch fresh numbers on the next page refresh
        return True, label

    def refill(self, args, who):
        what = str(args.get("what", ""))
        cap = self.cfg["capacity"]
        inv = json.loads(json.dumps((self.summary or {}).get("inventory") or {}))
        if not inv:
            self.watch_once()
            inv = json.loads(json.dumps((self.summary or {}).get("inventory") or {}))
        if not inv:
            return False, "Could not read the machine's counts"
        if what == "beans":
            patch, text = {"beans_g": cap["beans_g"]}, "beans"
        elif what.startswith("can") and what[3:].isdigit():
            i = int(what[3:]); arr = list(inv.get("canisters_g") or [0, 0, 0, 0])
            if not 0 <= i < len(arr):
                return False, "No such canister"
            arr[i] = cap["canister_g"]; patch = {"canisters_g": arr}
            text = (self.cfg["canister_names"] + [""] * 8)[i] or f"canister {i + 1}"
        elif what.startswith("syr") and what[3:].isdigit():
            i = int(what[3:]); arr = list(inv.get("syrups_ml") or [0, 0])
            if not 0 <= i < len(arr):
                return False, "No such syrup"
            arr[i] = cap["syrup_ml"]; patch = {"syrups_ml": arr}
            text = ((self.cfg["syrup_names"] + [""] * 4)[i] or f"syrup {i + 1}") + " syrup"
        elif what.startswith("cups"):
            size = what[4:]; cups = dict(inv.get("cups") or {})
            if size not in cups:
                return False, "No such cup size"
            n = args.get("count")
            cups[size] = int(n) if isinstance(n, (int, float)) and 0 <= n <= 1000 else cap["cups"]
            patch, text = {"cups": cups}, f"{size} oz cups ({cups[size]})"
        else:
            return False, "Unknown refill"
        with self.control_lock:
            try:
                status, _, data = self.m.request("POST", "/api/admin/inventory", json.dumps(patch).encode(),
                                                 {"Content-Type": "application/json", "X-Admin-Pin": self.cfg["owner_pin"]}, timeout=10)
            except MachineDown:
                return False, "The controller is not answering"
        if status != 200:
            return False, f"Controller said HTTP {status}"
        if self.summary:
            self.summary.setdefault("inventory", {}).update(patch)
        self.store.event("refill", f"Refilled {text} - {who}")
        return True, f"Refilled {text}"

    def control_state(self):
        if time.time() - self.summary_at > 5:
            self.watch_once()
        s = self.summary or {}
        try:
            menu = json.loads(self.m.cached_get("/api/menu")[2])
        except Exception:
            menu = {}
        events = self.store.q("SELECT at, kind, message FROM events WHERE kind IN ('control','refill','fault','cleared','stock','water') "
                              "ORDER BY id DESC LIMIT 15")
        return {
            "health": self.health(), "summary": s, "today": self.today(),
            "drinks": [{"id": d["id"], "name": d["name"], "sizes": list((d.get("prices") or {}).keys())} for d in menu.get("drinks", [])],
            "schedule": self.schedule, "scheduledOpen": scheduled_open(self.schedule),
            "capacity": self.cfg["capacity"], "lowStock": self.cfg["low_stock"],
            "canisterNames": self.cfg["canister_names"], "syrupNames": self.cfg["syrup_names"],
            "low": [t for _, t in self.low_items(s.get("inventory") or {})],
            "log": [{"at": a, "kind": k, "message": m} for a, k, m in events],
        }

    # ---- background: copy the sales log
    def sync_sales(self):
        pin = self.cfg.get("owner_pin")
        if not pin or self.pin_rejected:
            return 0
        st = (self.summary or {}).get("status") or {}
        if st.get("phase", "idle") != "idle":
            return 0                                          # never slow the controller mid-drink
        try:
            status, data = self.m.json("/api/admin/sales.csv", pin, timeout=30)
        except MachineDown:
            return 0
        if status == 401:
            self.pin_rejected = True
            return 0
        if status != 200:
            return 0
        added = self.store.add_sales(parse_sales_csv(data.decode("utf-8", "replace")))
        if added:
            log.info("copied %d new sales", added)
        return added

    # ---- background: keep a copy of the screens from the controller
    def sync_ui(self):
        for name in UI_FILES:
            try:
                status, _, data = self.m.request("GET", "/" + name, timeout=8)
            except MachineDown:
                return
            if status == 200 and data:
                tmp = os.path.join(self.ui_cache, name + ".tmp")
                with open(tmp, "wb") as f:
                    f.write(data)
                os.replace(tmp, os.path.join(self.ui_cache, name))

    def loop(self, every, fn, first_delay=2):
        def run():
            if self.stop.wait(first_delay):
                return
            while not self.stop.is_set():
                try:
                    fn()
                except Exception as e:
                    log.exception("%s failed: %s", fn.__name__, e)
                self.stop.wait(every())
        threading.Thread(target=run, daemon=True, name=fn.__name__).start()

    def start_background(self):
        self.loop(lambda: max(15, int(self.cfg["watch_seconds"])), self.watch_once, 1)
        self.loop(lambda: max(60, int(self.cfg["sync_minutes"]) * 60), self.sync_sales, 20)
        self.loop(lambda: 900, self.sync_ui, 10)
        self.loop(lambda: 30, self.schedule_tick, 15)
        self.loop(lambda: 60, self.report_tick, 30)

    # ---- data for the hub page
    def health(self):
        s = self.summary if time.time() - self.summary_at < 300 else None
        menu = None
        hit = self.m.cache.get("/api/menu")
        if hit:
            try:
                menu = json.loads(hit[1][2])
            except Exception:
                menu = None
        selling = s.get("selling") if s else (menu or {}).get("open")
        return {
            "name": self.cfg["machine_name"], "hub": VERSION, "online": self.m.online(),
            "lastContactSec": int(time.time() - self.m.last_ok) if self.m.last_ok else None,
            "selling": selling, "faults": (s or {}).get("faults") or (menu or {}).get("reason") or "",
            "water": self.state["water"], "phase": ((s or {}).get("status") or {}).get("phase"),
            "firmware": (s or {}).get("firmware"), "watching": bool(self.cfg.get("owner_pin")) and not self.pin_rejected,
            "pinRejected": self.pin_rejected, "hubUptimeMin": int((time.time() - self.started) / 60),
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
        }

    def report(self, days):
        since = int(time.time()) - days * 86400
        by_day = self.store.q("SELECT date(seen_at,'unixepoch','localtime') d, COUNT(*), SUM(price_cents) FROM sales "
                              "WHERE result='ok' AND seen_at>=? GROUP BY d ORDER BY d", (since,))
        by_drink = self.store.q("SELECT drink, COUNT(*), SUM(price_cents) FROM sales WHERE result='ok' AND seen_at>=? "
                                "GROUP BY drink ORDER BY COUNT(*) DESC", (since,))
        failed = self.store.q("SELECT COUNT(*) FROM sales WHERE result!='ok' AND seen_at>=?", (since,))[0][0]
        events = self.store.q("SELECT at, kind, message FROM events ORDER BY id DESC LIMIT 50")
        readings = self.store.q("SELECT at, espresso, boiler, cold, cabinet, water_ml FROM readings WHERE at>=? ORDER BY at", (int(time.time()) - 86400,))
        total = self.store.q("SELECT COUNT(*), COALESCE(SUM(price_cents),0) FROM sales WHERE result='ok'")[0]
        return {
            "health": self.health(), "days": days,
            "byDay": [{"day": d, "count": c, "cents": v or 0} for d, c, v in by_day],
            "byDrink": [{"drink": d, "count": c, "cents": v or 0} for d, c, v in by_drink],
            "failed": failed, "allTime": {"count": total[0], "cents": total[1]},
            "events": [{"at": a, "kind": k, "message": m} for a, k, m in events],
            "readings": [dict(zip(("at", "espresso", "boiler", "cold", "cabinet", "water_ml"), r)) for r in readings],
            "summary": self.summary,
        }

    def sales_csv(self):
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["machine", "controller_time", "hub_time", "drink", "size", "price", "payment", "result"])
        for t, d, sz, p, pay, res, seen in self.store.q("SELECT time,drink,size,price_cents,payment,result,seen_at FROM sales ORDER BY id"):
            w.writerow([self.cfg["machine_name"], t, time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(seen)), d, sz, f"{p / 100:.2f}", pay, res])
        return out.getvalue().encode()

    def fleet(self, pin):
        out = [dict(self.health(), url="(this machine)", today=self.today())]
        def fetch(url):
            url = url.rstrip("/")
            try:
                req = urllib.request.Request(url + "/hub/api/report?days=1", headers={"X-Hub-Pin": pin})
                with urllib.request.urlopen(req, timeout=6) as r:
                    rep = json.loads(r.read())
                h = rep["health"]
                h.update(url=url, today=(rep["byDay"][-1] if rep["byDay"] else {"count": 0, "cents": 0}))
                return h
            except urllib.error.HTTPError as e:
                return {"url": url, "name": url, "error": "Wrong hub PIN on that machine" if e.code == 401 else f"HTTP {e.code}"}
            except Exception as e:
                return {"url": url, "name": url, "error": f"Not reachable ({getattr(e, 'reason', e)})"}
        results = [None] * len(self.cfg["peers"])
        threads = []
        for i, u in enumerate(self.cfg["peers"]):
            t = threading.Thread(target=lambda i=i, u=u: results.__setitem__(i, fetch(u)))
            t.start(); threads.append(t)
        for t in threads:
            t.join(8)
        return out + [r or {"error": "timed out"} for r in results]

    def today(self):
        d = time.strftime("%Y-%m-%d")
        c, v = self.store.q("SELECT COUNT(*), COALESCE(SUM(price_cents),0) FROM sales WHERE result='ok' AND "
                            "date(seen_at,'unixepoch','localtime')=?", (d,))[0]
        return {"day": d, "count": c, "cents": v}

    # ---- PIN check with lockout
    def pin_ok(self, ip, given):
        count, until = self.bad_logins.get(ip, (0, 0))
        if time.time() < until:
            return None
        want = self.cfg.get("hub_pin") or ""
        if want and given and hmac.compare_digest(str(given), str(want)):
            self.bad_logins.pop(ip, None)
            return True
        count += 1
        self.bad_logins[ip] = (0, time.time() + 60) if count >= 5 else (count, 0)
        return False


# --------------------------------------------------------------------------- HTTP
def make_handler(hub):
    class H(BaseHTTPRequestHandler):
        server_version = "CoffeeHub/" + VERSION
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            log.debug("%s %s", self.client_address[0], fmt % args)

        def send(self, status, ctype, data, extra=None):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Admin-Pin, X-Counter-Pin, X-Hub-Pin")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS")
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def jsend(self, obj, status=200):
            self.send(status, "application/json", json.dumps(obj).encode())

        def body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n > 1_000_000:
                raise ValueError("body too large")
            return self.rfile.read(n) if n else None

        def do_OPTIONS(self):
            self.send(204, "text/plain", b"")

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            self.route("GET")

        def do_POST(self):
            self.route("POST")

        def do_PUT(self):
            self.route("PUT")

        def route(self, method):
            u = urlparse(self.path)
            p = u.path
            try:
                if p.startswith("/api/"):
                    return self.proxy(method, p, u.query)
                if p.startswith("/hub/api/"):
                    return self.hub_api(method, p, parse_qs(u.query))
                if method in ("GET", "HEAD"):
                    return self.static(p)
                self.jsend({"ok": False, "error": "Not found"}, 404)
            except (BrokenPipeError, ConnectionResetError):
                pass
            except Exception as e:
                log.exception("request failed: %s", e)
                try:
                    self.jsend({"ok": False, "error": "Hub error"}, 500)
                except Exception:
                    pass

        # ---- pass /api/* to the controller
        def proxy(self, method, path, query):
            body = self.body() if method in ("POST", "PUT") else None
            headers = {k: self.headers[k] for k in ("Content-Type", "X-Admin-Pin", "X-Counter-Pin") if self.headers.get(k)}
            try:
                if method == "GET" and path in CACHE_TTL and not query:
                    status, ctype, data = hub.m.cached_get(path)
                else:
                    if method != "GET":
                        hub.m.forget_cache()
                    status, ctype, data = hub.m.request(method, path + ("?" + query if query else ""), body, headers,
                                                        timeout=30 if path.endswith(".csv") else 8)
                    if method != "GET":
                        hub.m.forget_cache()
            except MachineDown:
                return self.jsend({"ok": False, "error": "The machine's controller is not answering. It may be restarting."}, 502)
            self.send(status, ctype, data)

        # ---- screens
        def static(self, p):
            if p in ("", "/"):
                p = "/index.html"
            elif p in ("/owner", "/owner/"):
                p = "/owner.html"
            elif p in ("/counter", "/counter/"):
                p = "/counter.html"
            elif p in ("/hub", "/hub/"):
                return self.file(os.path.join(HERE, "hub.html"))
            elif p in ("/control", "/control/", "/service", "/service/"):
                return self.file(os.path.join(HERE, "control.html"))
            elif p == "/control.webmanifest":
                return self.file(os.path.join(HERE, "control.webmanifest"))
            name = p.lstrip("/")
            if "/" in name or name.startswith("."):
                return self.jsend({"ok": False, "error": "Not found"}, 404)
            folders = (hub.ui_cache, hub.cfg["ui_dir"]) if hub.cfg.get("ui_from_controller") else (hub.cfg["ui_dir"], hub.ui_cache)
            for folder in folders:
                f = os.path.join(folder, name)
                if os.path.isfile(f):
                    return self.file(f)
            self.jsend({"ok": False, "error": "Not found"}, 404)

        def file(self, f):
            with open(f, "rb") as fh:
                data = fh.read()
            self.send(200, CTYPES.get(os.path.splitext(f)[1], "application/octet-stream"), data)

        # ---- hub's own API
        def hub_api(self, method, p, qs):
            if p == "/hub/api/health":
                return self.jsend(hub.health())
            if p == "/hub/api/kiosk":                     # public: lets the touchscreen know whether to offer the service corner
                return self.jsend({"serviceCorner": bool(hub.cfg.get("service_corner", True))})
            ok = hub.pin_ok(self.client_address[0], self.headers.get("X-Hub-Pin"))
            if ok is None:
                return self.jsend({"ok": False, "error": "Too many wrong PINs. Wait a minute."}, 429)
            if not ok:
                msg = "Set hub_pin or owner_pin in /etc/coffee-hub/config.json first" if not hub.cfg.get("hub_pin") else "Wrong PIN"
                return self.jsend({"ok": False, "error": msg}, 401)
            if p == "/hub/api/report":
                days = max(1, min(366, int((qs.get("days") or ["30"])[0])))
                return self.jsend(hub.report(days))
            if p == "/hub/api/sales.csv":
                return self.send(200, "text/csv", hub.sales_csv(),
                                 {"Content-Disposition": f'attachment; filename="{hub.cfg["machine_name"]}-sales.csv"'})
            if p == "/hub/api/fleet":
                return self.jsend({"machines": hub.fleet(self.headers.get("X-Hub-Pin"))})
            if p == "/hub/api/sync" and method == "POST":
                added = hub.sync_sales()
                return self.jsend({"ok": True, "added": added})
            if p == "/hub/api/control/state":
                return self.jsend(hub.control_state())
            if p == "/hub/api/control" and method == "POST":
                try:
                    req = json.loads(self.body() or b"{}")
                except ValueError:
                    return self.jsend({"ok": False, "error": "Not valid JSON"}, 400)
                where = "touchscreen" if self.client_address[0] in ("127.0.0.1", "::1") else self.client_address[0]
                ok, msg = hub.control(str(req.get("action", "")), req, where)
                return self.jsend({"ok": ok, "message" if ok else "error": msg}, 200 if ok else 400)
            if p == "/hub/api/schedule" and method == "POST":
                try:
                    sch = hub.save_schedule(json.loads(self.body() or b"{}"))
                except (ValueError, TypeError, AttributeError):
                    return self.jsend({"ok": False, "error": "Times must look like 07:30"}, 400)
                hub.store.event("control", "Opening hours " + ("on" if sch["enabled"] else "off"))
                return self.jsend({"ok": True, "schedule": sch})
            if p == "/hub/api/test-alert" and method == "POST":
                if not hub.cfg.get("ntfy_topic"):
                    return self.jsend({"ok": False, "error": "No ntfy_topic in config.json"}, 400)
                hub.notify("Test alert", "Alerts from this machine reach your phone.")
                return self.jsend({"ok": True})
            self.jsend({"ok": False, "error": "Not found"}, 404)

    return H


def main():
    ap = argparse.ArgumentParser(description="Coffee Hub")
    ap.add_argument("--config", default=os.environ.get("COFFEE_HUB_CONFIG", "/etc/coffee-hub/config.json"))
    ap.add_argument("--machine", dest="machine_url")
    ap.add_argument("--port", dest="listen_port", type=int)
    ap.add_argument("--data", dest="data_dir")
    ap.add_argument("--ui", dest="ui_dir")
    ap.add_argument("--owner-pin", dest="owner_pin")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    over = {k: getattr(a, k) for k in ("machine_url", "listen_port", "data_dir", "ui_dir", "owner_pin")}
    cfg = load_config(a.config, over)
    hub = Hub(cfg, a.config if os.path.exists(a.config) else None)
    hub.start_background()
    srv = ThreadingHTTPServer((cfg["listen_host"], int(cfg["listen_port"])), make_handler(hub))
    srv.daemon_threads = True

    def stop(*_):
        hub.stop.set()
        try:
            hub.store.checkpoint()
        finally:
            threading.Thread(target=srv.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, lambda *_: hub.reload())
    log.info("Coffee Hub %s on port %s, controller %s", VERSION, cfg["listen_port"], cfg["machine_url"])
    srv.serve_forever()


if __name__ == "__main__":
    main()
