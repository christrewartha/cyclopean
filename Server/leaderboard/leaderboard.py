"""
Cyclopean leaderboard service.

Tiny HTTP API (Python standard library only) backed by SQLite:

  GET  /api/v1/scores?limit=10&mode=arena   -> {"top": [{"rank", "name", "score"}...]}   best score per name
  POST /api/v1/scores                       -> {"rank": n, "best": n, "top": [...]}
       body: {"name": "ABC", "score": 1234, "kills": 9, "duration": 81.5, "version": "0.1", "mode": "arena"}

Each mode (game level) has its own board. "mode" defaults to "arena" (builds before 0.3 don't send it).
       header X-Signature: hex HMAC-SHA1(LEADERBOARD_SECRET, raw body)   (required if a secret is set)
  GET  /                         -> simple HTML table
  GET  /healthz                  -> "ok"
  GET  /ws/v1/match  (WebSocket) -> versus relay, see relay.py

The HMAC key ships inside the game, so it only stops casual junk, not a determined cheat. For a handful
of testers that is the right trade-off; the plausibility checks below catch the obviously impossible.

Config (environment):
  LEADERBOARD_SECRET   shared key; must match the game's CYCLOPS_LEADERBOARD_KEY (empty = no check)
  LEADERBOARD_DB       SQLite path (default /data/leaderboard.sqlite3)
  LEADERBOARD_PORT     listen port (default 8797)
"""

import hashlib
import hmac
import html
import json
import os
import re
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import relay

SECRET = os.environ.get("LEADERBOARD_SECRET", "").encode("utf-8")
DB_PATH = os.environ.get("LEADERBOARD_DB", "/data/leaderboard.sqlite3")
PORT = int(os.environ.get("LEADERBOARD_PORT", "8797"))

NAME_RE = re.compile(r"^[A-Z0-9]{3}$")
MAX_BODY = 4096
MAX_SCORE = 10_000_000
# Must stay in sync with the game's scoring: best case per kill is (100 + 50 one-shot + 100 headshot) x 4 combo.
MAX_POINTS_PER_KILL = 1000
MAX_KILLS_PER_SECOND = 3.0
# Level -> largest one-off bonus a run can add on top of kill points. Keep in sync with the game:
#   streets = AStreetSpawner finish bonus: 1000 + 500 x 5 lives + 10 x 180 s under par.
#   <level>_gun = the same level played with the hand gun instead of the optic blast (own board, same rules).
STREETS_BONUS = 1000 + 500 * 5 + 10 * 180
MODES = {"arena": 0, "streets": STREETS_BONUS, "streets_gun": STREETS_BONUS}
DEFAULT_MODE = "arena"
RATE_LIMIT_PER_MINUTE = 30

_db_lock = threading.Lock()
_rate = {}  # ip -> [window_start, count]


def db():
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.execute(
        """CREATE TABLE IF NOT EXISTS scores (
               id INTEGER PRIMARY KEY AUTOINCREMENT,
               name TEXT NOT NULL,
               score INTEGER NOT NULL,
               kills INTEGER NOT NULL,
               duration REAL NOT NULL,
               version TEXT NOT NULL DEFAULT '',
               ip TEXT NOT NULL DEFAULT '',
               created_at REAL NOT NULL)"""
    )
    # Migration: boards per mode (rows from before this are arena runs).
    columns = [row[1] for row in conn.execute("PRAGMA table_info(scores)")]
    if "mode" not in columns:
        conn.execute(f"ALTER TABLE scores ADD COLUMN mode TEXT NOT NULL DEFAULT '{DEFAULT_MODE}'")
        conn.commit()
    conn.execute("CREATE INDEX IF NOT EXISTS idx_scores_score ON scores(score DESC)")
    conn.execute("CREATE INDEX IF NOT EXISTS idx_scores_mode_score ON scores(mode, score DESC)")
    return conn


CONN = None


def top_scores(limit, mode):
    with _db_lock:
        rows = CONN.execute(
            "SELECT name, MAX(score) AS best FROM scores WHERE mode = ? GROUP BY name"
            " ORDER BY best DESC, MIN(created_at) LIMIT ?",
            (mode, limit),
        ).fetchall()
    return [{"rank": i + 1, "name": name, "score": best} for i, (name, best) in enumerate(rows)]


def insert_score(name, score, kills, duration, version, mode, ip):
    with _db_lock:
        CONN.execute(
            "INSERT INTO scores (name, score, kills, duration, version, mode, ip, created_at) VALUES (?,?,?,?,?,?,?,?)",
            (name, score, kills, duration, version, mode, ip, time.time()),
        )
        CONN.commit()
        best = CONN.execute("SELECT MAX(score) FROM scores WHERE name = ? AND mode = ?", (name, mode)).fetchone()[0]
        # Rank among players' bests on this mode's board (1 = top).
        higher = CONN.execute(
            "SELECT COUNT(*) FROM (SELECT MAX(score) AS b FROM scores WHERE mode = ? GROUP BY name) WHERE b > ?",
            (mode, best),
        ).fetchone()[0]
    return higher + 1, best


def rate_limited(ip):
    now = time.time()
    window = _rate.get(ip)
    if not window or now - window[0] > 60:
        _rate[ip] = [now, 1]
        return False
    window[1] += 1
    return window[1] > RATE_LIMIT_PER_MINUTE


def validate(payload):
    name = str(payload.get("name", "")).upper()
    if not NAME_RE.match(name):
        return None, "name must be 3 letters/digits"
    try:
        score = int(payload.get("score"))
        kills = int(payload.get("kills"))
        duration = float(payload.get("duration"))
    except (TypeError, ValueError):
        return None, "score/kills/duration must be numbers"
    if not (0 <= score <= MAX_SCORE) or kills < 0 or not (0 < duration < 24 * 3600):
        return None, "values out of range"
    mode = str(payload.get("mode") or DEFAULT_MODE).lower()
    if mode not in MODES:
        return None, "unknown mode"
    if score > kills * MAX_POINTS_PER_KILL + MODES[mode]:
        return None, "score not possible for that many kills"
    if kills > duration * MAX_KILLS_PER_SECOND + 5:
        return None, "too many kills for that duration"
    version = str(payload.get("version", ""))[:32]
    return (name, score, kills, duration, version, mode), None


def query_mode(url):
    mode = parse_qs(url.query).get("mode", [DEFAULT_MODE])[0].lower()
    return mode if mode in MODES else None


class Handler(BaseHTTPRequestHandler):
    server_version = "CyclopeanLeaderboard/1"

    def client_ip(self):
        # Behind Cloudflare Tunnel the real client address arrives in this header.
        return self.headers.get("CF-Connecting-IP") or self.client_address[0]

    def send_json(self, status, body):
        data = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urlparse(self.path)
        if url.path == relay.PATH and relay.is_upgrade(self):
            relay.handle(self, self.client_ip())
            return
        if url.path == "/healthz":
            data = b"ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if url.path == "/api/v1/scores":
            try:
                limit = max(1, min(100, int(parse_qs(url.query).get("limit", ["10"])[0])))
            except ValueError:
                limit = 10
            mode = query_mode(url)
            if mode is None:
                self.send_json(422, {"error": "unknown mode"})
                return
            self.send_json(200, {"top": top_scores(limit, mode)})
            return
        if url.path == "/":
            boards = ""
            for mode in MODES:
                rows = "".join(
                    f"<tr><td>{e['rank']}</td><td>{html.escape(e['name'])}</td><td>{e['score']:,}</td></tr>"
                    for e in top_scores(25, mode)
                )
                empty = "<tr><td>No scores yet</td></tr>"
                boards += f"<h2>{mode.upper()}</h2><table>{rows or empty}</table>"
            page = (
                "<!doctype html><meta charset=utf-8><title>Cyclopean leaderboard</title>"
                "<style>body{font-family:sans-serif;background:#0b0f16;color:#dde;max-width:28em;margin:3em auto}"
                "h1{color:#ff5a3c}h2{color:#9cd;margin-top:1.5em}td{padding:.25em 1em}"
                "tr:nth-child(odd){background:#141b26}</style>"
                f"<h1>CYCLOPEAN</h1>{boards}"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
            return
        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        if urlparse(self.path).path != "/api/v1/scores":
            self.send_json(404, {"error": "not found"})
            return
        ip = self.client_ip()
        if rate_limited(ip):
            self.send_json(429, {"error": "slow down"})
            return
        length = int(self.headers.get("Content-Length") or 0)
        if length <= 0 or length > MAX_BODY:
            self.send_json(400, {"error": "bad body size"})
            return
        raw = self.rfile.read(length)
        if SECRET:
            expected = hmac.new(SECRET, raw, hashlib.sha1).hexdigest()
            if not hmac.compare_digest(expected, self.headers.get("X-Signature", "").lower()):
                self.send_json(401, {"error": "bad signature"})
                return
        try:
            payload = json.loads(raw)
        except ValueError:
            self.send_json(400, {"error": "invalid json"})
            return
        values, error = validate(payload if isinstance(payload, dict) else {})
        if error:
            self.send_json(422, {"error": error})
            return
        rank, best = insert_score(*values, ip)
        mode = values[5]
        self.send_json(200, {"rank": rank, "best": best, "top": top_scores(10, mode)})

    def log_message(self, fmt, *args):
        print(f"{self.client_ip()} {fmt % args}", flush=True)


def main():
    global CONN
    os.makedirs(os.path.dirname(os.path.abspath(DB_PATH)), exist_ok=True)
    CONN = db()
    print(f"Cyclopean leaderboard on :{PORT}  db={DB_PATH}  signature={'on' if SECRET else 'OFF'}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
