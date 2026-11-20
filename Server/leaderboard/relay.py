"""
Cyclopean versus relay: two-player rooms over WebSocket (RFC 6455), Python standard library only.

Served by the leaderboard process at  GET /ws/v1/match  (Upgrade: websocket), so it shares the port and the
Cloudflare tunnel. The server doesn't run the game: each headset simulates its own world and the relay just passes
JSON messages between the two players in a room.

Client -> server (text frames, one JSON object each, "t" = type):
  {"t":"join", "room":"ABC", "name":"XYZ", "v":1}   first message; room/name are 3 x A-Z/0-9, v = protocol version
  {"t":"ping", "c":<anything>}                       -> {"t":"pong", "c":<same>}   (round-trip time, keep-alive)
  {"t":<anything else>, ...}                         relayed unchanged to the other player in the room
Server -> client:
  {"t":"joined", "room":"ABC", "slot":0|1, "peer":"XYZ"|null}
  {"t":"peer", "name":"XYZ"}      the other player arrived
  {"t":"left"}                    the other player went away (the room stays open for them to come back)
  {"t":"error", "msg":"..."}      then the connection is closed

Limits (a handful of testers, but it's on the internet): frame size, messages per second, rooms, idle timeout.
"""

import base64
import hashlib
import json
import re
import struct
import threading
import time

PROTOCOL = 1
PATH = "/ws/v1/match"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 4096          # bytes; poses are ~100
MAX_MSGS_PER_SEC = 120    # 30 Hz poses + events, with headroom
MAX_ROOMS = 50
IDLE_TIMEOUT = 45         # seconds without any frame (clients ping every few seconds)
CODE_RE = re.compile(r"^[A-Z0-9]{3}$")

_rooms = {}               # code -> [Conn | None, Conn | None]
_rooms_lock = threading.Lock()


class Closed(Exception):
    pass


class Conn:
    def __init__(self, rfile, sock, ip):
        self.rfile = rfile    # the HTTP handler's buffered reader (may already hold the first frame)
        self.sock = sock
        self.ip = ip
        self.send_lock = threading.Lock()
        self.room = None
        self.slot = None
        self.name = "???"
        self.window_start = time.monotonic()
        self.window_count = 0

    # ---- framing ----

    def _read(self, n):
        data = self.rfile.read(n)
        if not data or len(data) < n:
            raise Closed()
        return data

    def recv(self):
        """Next complete message as (opcode, payload). Reassembles fragments; answers pings."""
        message, message_op = b"", None
        while True:
            b1, b2 = self._read(2)
            fin, opcode = b1 & 0x80, b1 & 0x0F
            length = b2 & 0x7F
            if length == 126:
                length = struct.unpack("!H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack("!Q", self._read(8))[0]
            if not b2 & 0x80 or length > MAX_FRAME:
                raise Closed()  # clients must mask; nothing we accept is this big
            mask = self._read(4)
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(self._read(length)))

            if opcode == 0x8:      # close
                self.send_frame(0x8, payload[:2])
                raise Closed()
            if opcode == 0x9:      # ping
                self.send_frame(0xA, payload)
                continue
            if opcode == 0xA:      # pong
                continue
            if opcode != 0x0:      # start of a (possibly fragmented) data message
                message_op, message = opcode, b""
            message += payload
            if len(message) > MAX_FRAME:
                raise Closed()
            if fin:
                return message_op, message

    def send_frame(self, opcode, payload):
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x80 | opcode, n)
        elif n < 65536:
            header = struct.pack("!BBH", 0x80 | opcode, 126, n)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 127, n)
        with self.send_lock:
            try:
                self.sock.sendall(header + payload)
            except OSError:
                pass  # the reader thread notices and cleans up

    def send(self, obj):
        self.send_frame(0x1, json.dumps(obj, separators=(",", ":")).encode("utf-8"))

    def rate_ok(self):
        now = time.monotonic()
        if now - self.window_start >= 1.0:
            self.window_start, self.window_count = now, 0
        self.window_count += 1
        return self.window_count <= MAX_MSGS_PER_SEC


# ---- rooms ----

def _join(conn, room, name):
    with _rooms_lock:
        slots = _rooms.get(room)
        if slots is None:
            if len(_rooms) >= MAX_ROOMS:
                return "server full"
            slots = _rooms[room] = [None, None]
        if None not in slots:
            return "room full"
        conn.slot = slots.index(None)
        slots[conn.slot] = conn
        conn.room, conn.name = room, name
        peer = slots[1 - conn.slot]
    conn.send({"t": "joined", "room": room, "slot": conn.slot, "peer": peer.name if peer else None})
    if peer:
        peer.send({"t": "peer", "name": name})
    return None


def _leave(conn):
    if conn.room is None:
        return
    with _rooms_lock:
        slots = _rooms.get(conn.room)
        peer = None
        if slots and slots[conn.slot] is conn:
            slots[conn.slot] = None
            peer = slots[1 - conn.slot]
            if peer is None:
                del _rooms[conn.room]
        conn.room = None
    if peer:
        peer.send({"t": "left"})


def _peer(conn):
    with _rooms_lock:
        slots = _rooms.get(conn.room)
        return slots[1 - conn.slot] if slots else None


def stats():
    with _rooms_lock:
        return {"rooms": len(_rooms), "players": sum(1 for s in _rooms.values() for c in s if c)}


# ---- entry point from the HTTP handler ----

def is_upgrade(handler):
    return handler.headers.get("Upgrade", "").lower() == "websocket"


def handle(handler, ip):
    """Takes over a BaseHTTPRequestHandler connection whose request is a WebSocket upgrade to PATH."""
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key:
        handler.send_error(400, "missing Sec-WebSocket-Key")
        return
    accept = base64.b64encode(hashlib.sha1((key + GUID).encode("ascii")).digest()).decode("ascii")
    handler.protocol_version = "HTTP/1.1"  # the default HTTP/1.0 status line isn't a valid upgrade response
    handler.send_response(101, "Switching Protocols")
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept)
    handler.end_headers()
    handler.wfile.flush()
    handler.close_connection = True

    sock = handler.connection
    sock.settimeout(IDLE_TIMEOUT)
    conn = Conn(handler.rfile, sock, ip)
    try:
        while True:
            opcode, payload = conn.recv()
            if opcode != 0x1 or not conn.rate_ok():
                raise Closed()
            try:
                msg = json.loads(payload)
                kind = msg["t"]
            except (ValueError, KeyError, TypeError):
                raise Closed()

            if kind == "ping":
                conn.send({"t": "pong", "c": msg.get("c")})
            elif conn.room is None:
                # The only thing allowed before joining.
                room, name = str(msg.get("room", "")), str(msg.get("name", ""))
                if kind != "join" or not CODE_RE.match(room) or not CODE_RE.match(name):
                    conn.send({"t": "error", "msg": "bad join"})
                    raise Closed()
                if msg.get("v") != PROTOCOL:
                    conn.send({"t": "error", "msg": "update the game"})
                    raise Closed()
                error = _join(conn, room, name)
                if error:
                    conn.send({"t": "error", "msg": error})
                    raise Closed()
                print(f"relay: {name} joined {room} as slot {conn.slot} from {ip} ({stats()})", flush=True)
            else:
                peer = _peer(conn)
                if peer:
                    peer.send_frame(0x1, payload)  # unchanged
    except (Closed, OSError):
        pass
    finally:
        if conn.room:
            print(f"relay: {conn.name} left {conn.room}", flush=True)
        _leave(conn)
