#!/usr/bin/env python3
"""Crysis Coop relay.

The game itself runs on the host's PC (AI, physics, story). This service only
forwards the game's UDP datagrams between the host and his friends, so nobody
has to open or forward ports, and keeps the co-op campaigns' checkpoints for
the players who want them in the cloud. It never runs the game.

Coop.dll talks to it over a WebSocket (wss://..., behind Caddy on port 443:
the only way in, the provider's firewall drops everything else):

  friend's game --UDP--> Coop.dll (127.0.0.1) ==wss==> relay ==wss==> Coop.dll
                                                                   --UDP--> host's game (127.0.0.1:64087)

Players: every Coop.dll has a random secret key (16 bytes, kept on the
player's PC). The relay only keeps its public id, the first 8 bytes of its
SHA-256. A host keeps the same 6 digit code for as long as he plays.

WebSocket messages (binary, integers little endian). Protocol 2:
  host   -> relay  1 REGISTER   ver=2:u8 key:16
  relay  -> host   2 REGISTERED code:u32 id:8
  both   -> relay  3 PING       (echoed as it came: timestamps)
  relay  -> host   4 DATA       client:u16 payload
  host   -> relay  5 DATA       client:u16 payload
  relay  -> host   6 CLOSE      client:u16            (friend gone)
  relay  -> any    7 ERROR      code:u8
  friend -> relay  8 JOIN       ver=2:u8 code:u32 token:8 key:16
                                (the same token: back as the same client within CLIENT_RESUME)
  relay  -> friend 9 JOINED     host_state:u8
  friend -> relay 10 DATA       payload
  relay  -> friend 11 DATA      payload
  friend -> relay 12 CANDIDATES token:8 count:u8 (ip:4 port:u16)*
  relay  -> host  13 CANDIDATES client:u16 token:8 count:u8 (ip:4 port:u16)*
  host   -> relay 14 CANDIDATES client:u16 count:u8 (ip:4 port:u16)*
  relay  -> friend 15 CANDIDATES count:u8 (ip:4 port:u16)*
  host   -> relay 16 HOST_STATE state:u8   1 restarting (a new level / a checkpoint loads), 2 ready
  relay  -> friend 16 HOST_STATE state:u8  the same, 3 host connection lost (waiting for him), 4 game closed
  relay  -> host  17 FRIEND     client:u16 id:8   (who a client is)

  cloud (a connection of its own):
  any    -> relay 20 HELLO      ver=2:u8 key:16
  relay  -> any   21 WELCOME    id:8
  any    -> relay 22 UPLOAD     campaign:8 size:u32     a checkpoint (see "blob" below) follows
  relay  -> any   23 GO
  any    -> relay 24 CHUNK      bytes (at most 48 KB each)
  any    -> relay 25 DONE
  relay  -> any   26 STORED
  any    -> relay 27 LIST
  relay  -> any   28 CAMPAIGNS  utf-8 lines, tab separated: campaign (hex) own|member stamp level
                                checkpoint name host has_previous(0/1)
  any    -> relay 29 DOWNLOAD   campaign:8 which:u8 (0 last checkpoint, 1 the one before)
  relay  -> any   30 FILE       size:u32, then 24 CHUNK..., then 25 DONE
  any    -> relay 33 DELETE     campaign:8   (its owner: gone; a member: no longer his)
  relay  -> any   34 DELETED

  blob: "CCSV" ver=1:u32 progress_len:u32 progress (coop_progress.txt) save_len:u32 save (the game's savegame)
  A campaign belongs to the player who stored it first; the players listed in
  its progress file (player=id:...) may load and store it too.

Errors: 1 relay full, 2 other version, 3 no such game, 4 game full, 8 not
allowed, 9 too big / no room, 10 not found, 11 too often, 12 bad data.

Protocol 1 (mod 0.2 / 0.3) still works: REGISTER ver=1 prev_code:u16 prev_secret:8
-> REGISTERED code:u16 secret:8; JOIN ver=1 code:u16 token:8 -> JOINED. Its codes
are 64101-64164.

Direct path: the relay is also where the host and a friend swap the UDP
addresses they can be reached at (their local ones and the public one a STUN
server reported). Both then send UDP to each other (NAT hole punching); when
that gets through, the game traffic goes straight between them and the relay
stays as the fallback.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import logging
import os
import random
import struct
import time

MAGIC_BLOB = b"CCSV"

SESSION_TIMEOUT = 45.0      # s without the host and nobody waiting: the game ends
HOST_WAIT = 300.0           # s the friends wait for a host who lost his connection
CLIENT_TIMEOUT = 60.0       # s without a packet or ping from a friend: forgotten
CLIENT_RESUME = 20.0        # s a friend whose connection broke may come back as the same client
MAX_CLIENTS = 8             # friends per game
MAX_PAYLOAD = 1500
MAX_SESSIONS_PER_IP = 4     # hosts behind one address
MAX_NEW_IDS_PER_IP = 20     # new players (codes) per address and hour
CODE_EXPIRY = 60 * 86400    # a code nobody hosted with for 60 days is free again
RATE_LIMIT = 2_000_000      # bytes/s per game and direction (a coop game needs ~50 KB/s)
WS_BACKLOG_LIMIT = 1 << 20  # bytes queued to a slow WebSocket before datagrams are dropped
MAX_MESSAGE = 65536 + 16

MAX_BLOB = 16 << 20         # one checkpoint (a savegame is ~1.5 MB)
CHUNK = 48 * 1024
MAX_CAMPAIGNS = 10          # stored campaigns per player
DISK_CAP = 2 << 30          # all checkpoints together
UPLOAD_INTERVAL = 10.0      # s between two checkpoints of one player
CAMPAIGN_EXPIRY = 180 * 86400
MAX_CLOUD_PER_IP = 4        # cloud connections at once per address

STATE_RESTARTING, STATE_READY, STATE_GONE, STATE_CLOSED = 1, 2, 3, 4

E_FULL, E_VERSION, E_NO_GAME, E_GAME_FULL = 1, 2, 3, 4
E_DENIED, E_TOO_BIG, E_NOT_FOUND, E_TOO_OFTEN, E_BAD = 8, 9, 10, 11, 12

log = logging.getLogger("coop_relay")


def player_id(key):
    return hashlib.sha256(key).digest()[:8]


# --------------------------------------------------------------------------
# minimal WebSocket server side (RFC 6455): binary messages, ping/pong, close
class WebSocket:
    GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, reader, writer, peer):
        self.reader = reader
        self.writer = writer
        self.peer = peer
        self.closed = False

    async def handshake(self):
        head = await asyncio.wait_for(self.reader.readuntil(b"\r\n\r\n"), 10)
        lines = head.decode("latin-1").split("\r\n")
        headers = {}
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        # the real address of the player (Caddy forwards it)
        fwd = headers.get("x-forwarded-for")
        if fwd:
            self.peer = (fwd.split(",")[0].strip(), 0)
        key = headers.get("sec-websocket-key")
        if not key or "websocket" not in headers.get("upgrade", "").lower():
            self.writer.write(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            await self.writer.drain()
            return False
        accept = base64.b64encode(hashlib.sha1(key.encode() + self.GUID).digest())
        self.writer.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          b"Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
        await self.writer.drain()
        return True

    async def recv(self):
        """One complete message (bytes), or None when the connection ends."""
        message = b""
        while True:
            b0, b1 = await self.reader.readexactly(2)
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", await self.reader.readexactly(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", await self.reader.readexactly(8))[0]
            if length > MAX_MESSAGE or len(message) + length > MAX_MESSAGE:
                return None
            mask = await self.reader.readexactly(4) if b1 & 0x80 else None
            data = await self.reader.readexactly(length)
            if mask and length:
                m = (mask * (length // 4 + 1))[:length]
                data = (int.from_bytes(data, "big") ^ int.from_bytes(m, "big")).to_bytes(length, "big")
            if opcode == 0x8:       # close
                return None
            if opcode == 0x9:       # ping
                self._frame(0xA, data)
                continue
            if opcode == 0xA:       # pong
                continue
            message += data
            if b0 & 0x80:           # FIN
                return message

    def _frame(self, opcode, data):
        if self.closed:
            return
        n = len(data)
        if n < 126:
            head = bytes([0x80 | opcode, n])
        elif n < 65536:
            head = bytes([0x80 | opcode, 126]) + struct.pack(">H", n)
        else:
            head = bytes([0x80 | opcode, 127]) + struct.pack(">Q", n)
        self.writer.write(head + data)

    def send(self, data):
        # datagrams: drop rather than queue without end for a stalled peer
        if self.writer.transport.get_write_buffer_size() > WS_BACKLOG_LIMIT:
            return
        self._frame(0x2, data)

    async def send_all(self, data):
        """A message that must arrive (files): waits for the peer instead of dropping."""
        self._frame(0x2, data)
        await self.writer.drain()

    def close(self):
        if not self.closed:
            self._frame(0x8, b"")
            self.closed = True
            self.writer.close()


# --------------------------------------------------------------------------
class Store:
    """What survives a restart of the service: the players' codes and the
    campaigns' checkpoints, under the data directory."""

    def __init__(self, root):
        self.root = root
        self.campaign_dir = os.path.join(root, "campaigns")
        os.makedirs(self.campaign_dir, exist_ok=True)
        self.codes_path = os.path.join(root, "codes.json")
        self.codes = {}         # player id (hex) -> [code, last used]
        self.by_code = {}
        self.new_ids = {}       # address -> [times of new codes]
        try:
            with open(self.codes_path, "r", encoding="utf-8") as f:
                self.codes = {k: list(v) for k, v in json.load(f).items()}
        except (OSError, ValueError):
            pass
        self.by_code = {v[0]: k for k, v in self.codes.items()}
        self.campaigns = {}     # campaign (hex) -> meta
        for name in os.listdir(self.campaign_dir):
            meta = self.read_meta(name)
            if meta:
                self.campaigns[name] = meta
        log.info("store %s: %d players, %d campaigns, %.1f MB", root, len(self.codes), len(self.campaigns),
                 self.disk_used() / 1e6)

    @staticmethod
    def write_json(path, data):
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f)
        os.replace(tmp, path)

    # ---- codes
    def code_for(self, pid, ip):
        entry = self.codes.get(pid)
        now = time.time()
        if entry is None:
            times = [t for t in self.new_ids.get(ip, []) if now - t < 3600]
            if len(times) >= MAX_NEW_IDS_PER_IP:
                return None
            self.new_ids[ip] = times + [now]
            while True:
                code = random.randint(100000, 999999)
                if code not in self.by_code:
                    break
            entry = self.codes[pid] = [code, now]
            self.by_code[code] = pid
        entry[1] = now
        self.write_json(self.codes_path, self.codes)
        return entry[0]

    def expire_codes(self):
        now = time.time()
        old = [pid for pid, (code, used) in self.codes.items() if now - used > CODE_EXPIRY]
        for pid in old:
            self.by_code.pop(self.codes.pop(pid)[0], None)
        if old:
            self.write_json(self.codes_path, self.codes)

    # ---- campaigns
    def path(self, campaign, name):
        return os.path.join(self.campaign_dir, campaign, name)

    def read_meta(self, campaign):
        if len(campaign) != 16 or any(c not in "0123456789abcdef" for c in campaign):
            return None
        try:
            with open(self.path(campaign, "meta.json"), "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def disk_used(self):
        return sum(m.get("size", 0) + m.get("prev_size", 0) for m in self.campaigns.values())

    def may_read(self, pid, meta):
        return meta["owner"] == pid or pid in meta.get("members", [])

    def list_for(self, pid):
        out = []
        for campaign, m in self.campaigns.items():
            if self.may_read(pid, m):
                out.append("\t".join([campaign, "own" if m["owner"] == pid else "member", str(m.get("stamp", 0)),
                                      m.get("level", ""), m.get("checkpoint", ""), m.get("name", ""), m.get("host", ""),
                                      "1" if m.get("prev_size") else "0"]))
        return "\n".join(out)

    def delete(self, campaign):
        folder = os.path.join(self.campaign_dir, campaign)
        for name in ("last.bin", "prev.bin", "meta.json", "upload.tmp"):
            try:
                os.remove(os.path.join(folder, name))
            except OSError:
                pass
        try:
            os.rmdir(folder)
        except OSError:
            pass
        self.campaigns.pop(campaign, None)

    def expire_campaigns(self):
        now = time.time()
        for campaign, m in list(self.campaigns.items()):
            if now - m.get("updated", now) > CAMPAIGN_EXPIRY:
                log.info("campaign %s expired", campaign)
                self.delete(campaign)

    def store(self, campaign, pid, tmp_path, size):
        """A complete upload: checked, becomes the last checkpoint (the last one becomes the one before)."""
        try:
            with open(tmp_path, "rb") as f:
                head = f.read(12)
                if len(head) < 12 or head[:4] != MAGIC_BLOB:
                    return E_BAD
                plen = struct.unpack_from("<I", head, 8)[0]
                if plen > 65536 or 12 + plen + 4 > size:
                    return E_BAD
                progress = f.read(plen).decode("utf-8", "replace")
                slen = struct.unpack("<I", f.read(4))[0]
                if 12 + plen + 4 + slen != size:
                    return E_BAD
        except OSError:
            return E_BAD
        info = {}
        members = set()
        for line in progress.splitlines():
            key, _, value = line.partition("=")
            if key == "player":
                who = value.split("\t", 1)[0]
                if who.startswith("id:") and len(who) == 19:
                    members.add(who[3:].lower())
            elif key in ("campaign", "name", "level", "checkpoint", "stamp", "hostname"):
                info[key] = value[:64]
        if info.get("campaign", "").lower() != campaign:
            return E_BAD
        meta = self.campaigns.get(campaign)
        if meta is None:
            if sum(1 for m in self.campaigns.values() if m["owner"] == pid) >= MAX_CAMPAIGNS:
                return E_TOO_BIG
            meta = {"owner": pid, "members": [], "created": time.time()}
        elif not (meta["owner"] == pid or pid in meta.get("members", [])):
            return E_DENIED
        last = self.path(campaign, "last.bin")
        if os.path.exists(last):
            os.replace(last, self.path(campaign, "prev.bin"))
            meta["prev_size"] = meta.get("size", 0)
            meta["prev"] = {k: meta.get(k) for k in ("stamp", "level", "checkpoint")}
        os.replace(tmp_path, last)
        meta["members"] = sorted((set(meta.get("members", [])) | members) - {meta["owner"]})
        meta.update({"size": size, "updated": time.time(), "name": info.get("name", ""), "level": info.get("level", ""),
                     "checkpoint": info.get("checkpoint", ""), "host": info.get("hostname", "")})
        try:
            meta["stamp"] = int(info.get("stamp", "0"))
        except ValueError:
            meta["stamp"] = 0
        self.write_json(self.path(campaign, "meta.json"), meta)
        self.campaigns[campaign] = meta
        return 0


# --------------------------------------------------------------------------
class Session:
    def __init__(self, relay, code, host_ip, v2):
        self.relay = relay
        self.code = code
        self.v2 = v2
        self.secret = os.urandom(8)
        self.host_ip = host_ip
        self.host_ws = None
        self.host_state = STATE_READY
        self.last_seen = time.monotonic()
        self.clients = {}           # client id -> [WebSocket or None, last seen, player id or None]
        self.by_peer = {}           # WebSocket -> client id
        self.tokens = {}            # a friend's token -> client id (to come back)
        self.next_id = 1
        self.bytes_in = 0
        self.bytes_out = 0
        self.budget_in = RATE_LIMIT
        self.budget_out = RATE_LIMIT
        self.budget_time = time.monotonic()

    def refill(self):
        now = time.monotonic()
        add = (now - self.budget_time) * RATE_LIMIT
        self.budget_time = now
        self.budget_in = min(RATE_LIMIT, self.budget_in + add)
        self.budget_out = min(RATE_LIMIT, self.budget_out + add)

    def add_client(self, ws, pid):
        if len(self.clients) >= MAX_CLIENTS:
            return None
        cid = self.next_id
        self.next_id = self.next_id % 65535 + 1
        self.clients[cid] = [ws, time.monotonic(), pid]
        self.by_peer[ws] = cid
        log.info("game %d: friend %s joined as client %d", self.code, ws.peer[0], cid)
        self.tell_host_who(cid)
        return cid

    def tell_host_who(self, cid):
        entry = self.clients.get(cid)
        if self.host_ws and entry and entry[2]:
            self.host_ws.send(bytes([17]) + struct.pack("<H", cid) + entry[2])

    def resume_client(self, token, ws):
        cid = self.tokens.get(token)
        entry = self.clients.get(cid)
        if not entry:
            return None
        old = entry[0]
        if old is not None:
            self.by_peer.pop(old, None)
            old.close()
        entry[0] = ws
        entry[1] = time.monotonic()
        self.by_peer[ws] = cid
        return cid

    def detach_client(self, cid, ws):
        entry = self.clients.get(cid)
        if entry and entry[0] is ws:
            self.by_peer.pop(ws, None)
            entry[0] = None
            entry[1] = time.monotonic()

    def drop_client(self, cid, why):
        entry = self.clients.pop(cid, None)
        if entry:
            self.by_peer.pop(entry[0], None)
            for t in [t for t, c in self.tokens.items() if c == cid]:
                del self.tokens[t]
            log.info("game %d: client %d %s", self.code, cid, why)
            if self.host_ws:
                self.host_ws.send(bytes([6]) + struct.pack("<H", cid))

    def set_host_state(self, state):
        if state == self.host_state:
            return
        self.host_state = state
        log.info("game %d: host %s", self.code, {1: "restarting", 2: "ready", 3: "connection lost", 4: "gone"}.get(state))
        for ws, _, _ in self.clients.values():
            if ws is not None:
                ws.send(bytes([16, state]))

    def friends_waiting(self):
        return any(ws is not None for ws, _, _ in self.clients.values())

    def from_friend(self, cid, data):
        """A datagram of a friend's game, for the host's game."""
        self.refill()
        if not self.host_ws or self.budget_in < len(data) or len(data) > MAX_PAYLOAD:
            return
        self.budget_in -= len(data)
        self.bytes_in += len(data)
        self.host_ws.send(bytes([4]) + struct.pack("<H", cid) + data)

    def from_host(self, cid, data):
        """A datagram of the host's game, for one friend."""
        entry = self.clients.get(cid)
        self.refill()
        if not entry or self.budget_out < len(data):
            return
        self.budget_out -= len(data)
        self.bytes_out += len(data)
        if entry[0] is not None:
            entry[0].send(bytes([11]) + data)


class Relay:
    def __init__(self, ws_bind, ws_port, first_code, last_code, data):
        self.ws_bind = ws_bind
        self.ws_port = ws_port
        self.v1_codes = list(range(first_code, last_code + 1))
        self.sessions = {}      # code -> Session
        self.store = Store(data)
        self.cloud_ips = {}     # address -> cloud connections open
        self.last_upload = {}   # player id -> time

    def new_session(self, host_ip, code, v2):
        if sum(1 for s in self.sessions.values() if s.host_ip == host_ip) >= MAX_SESSIONS_PER_IP:
            log.warning("too many games from %s", host_ip)
            return None
        s = Session(self, code, host_ip, v2)
        self.sessions[code] = s
        log.info("game %d opened for host %s", code, host_ip)
        return s

    def end_session(self, s, why):
        log.info("game %d closed (%s), %d bytes to the host, %d to friends", s.code, why, s.bytes_in, s.bytes_out)
        self.sessions.pop(s.code, None)
        for ws, _, _ in list(s.clients.values()):
            if ws is not None:
                ws.send(bytes([16, STATE_CLOSED]))
                ws.close()

    # ---- hosts
    def register(self, ws, msg):
        """REGISTER: the session this host runs, or None (an error was sent)."""
        ver = msg[1]
        if ver == 2 and len(msg) >= 18:
            pid = player_id(bytes(msg[2:18]))
            code = self.store.code_for(pid.hex(), ws.peer[0])
            if code is None:
                ws.send(bytes([7, E_FULL]))
                return None
            s = self.sessions.get(code)
            if s:
                if s.host_ws is not None and s.host_ws is not ws:
                    s.host_ws.close()       # the same player again: his new connection counts
                log.info("game %d: host back", code)
            else:
                s = self.new_session(ws.peer[0], code, True)
                if s is None:
                    ws.send(bytes([7, E_FULL]))
                    return None
            s.host_ws = ws
            s.last_seen = time.monotonic()
            ws.send(bytes([2]) + struct.pack("<I", code) + pid)
            for cid in s.clients:
                s.tell_host_who(cid)
            return s
        if ver == 1 and len(msg) >= 12:
            prev_code = struct.unpack_from("<H", msg, 2)[0]
            prev = self.sessions.get(prev_code)
            if prev and not prev.v2 and prev.secret == bytes(msg[4:12]) and prev.host_ws is None:
                s = prev
                log.info("game %d: host back", prev_code)
            else:
                free = [c for c in self.v1_codes if c not in self.sessions]
                s = None
                if free:
                    s = self.new_session(ws.peer[0], prev_code if prev_code in free else free[0], False)
            if s is None:
                ws.send(bytes([7, E_FULL]))
                return None
            s.host_ws = ws
            s.last_seen = time.monotonic()
            ws.send(bytes([2]) + struct.pack("<H", s.code) + s.secret)
            return s
        ws.send(bytes([7, E_VERSION]))
        return None

    def join(self, ws, msg):
        """JOIN: (session, client id), or None (an error was sent)."""
        ver = msg[1]
        if ver == 2 and len(msg) >= 30:
            code = struct.unpack_from("<I", msg, 2)[0]
            token = bytes(msg[6:14])
            pid = player_id(bytes(msg[14:30]))
        elif ver == 1 and len(msg) >= 4:
            code = struct.unpack_from("<H", msg, 2)[0]
            token = bytes(msg[4:12]) if len(msg) >= 12 else None
            pid = None
        else:
            ws.send(bytes([7, E_VERSION]))
            return None
        s = self.sessions.get(code)
        if not s or s.v2 != (ver == 2) or (not s.v2 and s.host_ws is None):
            ws.send(bytes([7, E_NO_GAME]))
            return None
        cid = s.resume_client(token, ws) if token else None
        if cid is not None:
            log.info("game %d: client %d back", s.code, cid)
        else:
            cid = s.add_client(ws, pid)
            if cid is None:
                ws.send(bytes([7, E_GAME_FULL]))
                return None
            if token:
                s.tokens[token] = cid
        ws.send(bytes([9, s.host_state if s.host_ws else STATE_GONE]) if s.v2 else b"\x09")
        return s, cid

    # ---- cloud
    async def cloud(self, ws, pid):
        """The cloud requests of one connection (after HELLO)."""
        hexid = pid.hex()
        upload = None       # [campaign, size, received, file, tmp path]
        try:
            while True:
                msg = await ws.recv()
                if msg is None:
                    break
                if not msg:
                    continue
                op = msg[0]
                if op == 3:
                    ws.send(msg[:16])
                elif op == 22 and len(msg) >= 13 and upload is None:
                    campaign = bytes(msg[1:9]).hex()
                    size = struct.unpack_from("<I", msg, 9)[0]
                    meta = self.store.campaigns.get(campaign)
                    if meta and not self.store.may_read(hexid, meta):
                        await ws.send_all(bytes([7, E_DENIED]))
                    elif size > MAX_BLOB or self.store.disk_used() + size > DISK_CAP:
                        await ws.send_all(bytes([7, E_TOO_BIG]))
                    elif time.monotonic() - self.last_upload.get(hexid, -1e9) < UPLOAD_INTERVAL:
                        await ws.send_all(bytes([7, E_TOO_OFTEN]))
                    else:
                        self.last_upload[hexid] = time.monotonic()
                        os.makedirs(os.path.join(self.store.campaign_dir, campaign), exist_ok=True)
                        tmp = self.store.path(campaign, "upload.tmp")
                        upload = [campaign, size, 0, open(tmp, "wb"), tmp]
                        await ws.send_all(bytes([23]))
                elif op == 24 and upload is not None:
                    upload[2] += len(msg) - 1
                    if upload[2] > upload[1]:
                        upload[3].close()
                        os.remove(upload[4])
                        upload = None
                        await ws.send_all(bytes([7, E_BAD]))
                        continue
                    upload[3].write(msg[1:])
                elif op == 25 and upload is not None:
                    campaign, size, got, f, tmp = upload
                    upload = None
                    f.close()
                    err = self.store.store(campaign, hexid, tmp, got) if got == size else E_BAD
                    if err:
                        try:
                            os.remove(tmp)
                        except OSError:
                            pass
                        await ws.send_all(bytes([7, err]))
                    else:
                        log.info("campaign %s: checkpoint %s stored by %s (%d bytes)", campaign,
                                 self.store.campaigns[campaign].get("checkpoint"), hexid, size)
                        await ws.send_all(bytes([26]))
                elif op == 27:
                    await ws.send_all(bytes([28]) + self.store.list_for(hexid).encode("utf-8"))
                elif op == 29 and len(msg) >= 10:
                    campaign = bytes(msg[1:9]).hex()
                    meta = self.store.campaigns.get(campaign)
                    path = self.store.path(campaign, "prev.bin" if msg[9] else "last.bin")
                    if not meta or not self.store.may_read(hexid, meta):
                        await ws.send_all(bytes([7, E_NOT_FOUND]))
                        continue
                    try:
                        with open(path, "rb") as f:
                            data = f.read()
                    except OSError:
                        await ws.send_all(bytes([7, E_NOT_FOUND]))
                        continue
                    await ws.send_all(bytes([30]) + struct.pack("<I", len(data)))
                    for i in range(0, len(data), CHUNK):
                        await ws.send_all(bytes([24]) + data[i:i + CHUNK])
                    await ws.send_all(bytes([25]))
                elif op == 33 and len(msg) >= 9:
                    campaign = bytes(msg[1:9]).hex()
                    meta = self.store.campaigns.get(campaign)
                    if not meta or not self.store.may_read(hexid, meta):
                        await ws.send_all(bytes([7, E_NOT_FOUND]))
                    elif meta["owner"] == hexid:
                        self.store.delete(campaign)
                        log.info("campaign %s deleted by its owner", campaign)
                        await ws.send_all(bytes([34]))
                    else:
                        meta["members"] = [m for m in meta.get("members", []) if m != hexid]
                        self.store.write_json(self.store.path(campaign, "meta.json"), meta)
                        await ws.send_all(bytes([34]))
        finally:
            if upload is not None:
                upload[3].close()
                try:
                    os.remove(upload[4])
                except OSError:
                    pass

    # ---- one WebSocket: a host, a friend or a cloud client
    async def on_websocket(self, reader, writer):
        ws = WebSocket(reader, writer, writer.get_extra_info("peername"))
        session = None
        role = None
        cid = None
        try:
            if not await ws.handshake():
                return
            while True:
                msg = await ws.recv()
                if msg is None:
                    break
                if not msg:
                    continue
                op = msg[0]
                if op == 3:
                    ws.send(msg[:16])       # echoed as it came (timestamps)
                    if role == "host":
                        session.last_seen = time.monotonic()
                    elif role == "friend" and cid in session.clients:
                        # on a direct path the friend sends no data here, only pings
                        session.clients[cid][1] = time.monotonic()
                elif role == "host" and op == 5 and len(msg) >= 3:
                    session.last_seen = time.monotonic()
                    session.from_host(struct.unpack_from("<H", msg, 1)[0], msg[3:])
                elif role == "friend" and op == 10:
                    if cid in session.clients:
                        session.clients[cid][1] = time.monotonic()
                        session.from_friend(cid, msg[1:])
                elif role == "host" and op == 16 and len(msg) >= 2 and msg[1] in (STATE_RESTARTING, STATE_READY):
                    session.last_seen = time.monotonic()
                    session.set_host_state(msg[1])
                elif role == "friend" and op == 12 and len(msg) >= 10:
                    if session.host_ws:
                        session.host_ws.send(bytes([13]) + struct.pack("<H", cid) + msg[1:64])
                elif role == "host" and op == 14 and len(msg) >= 4:
                    entry = session.clients.get(struct.unpack_from("<H", msg, 1)[0])
                    if entry and entry[0] is not None:
                        entry[0].send(bytes([15]) + msg[3:64])
                elif role is None and op == 1 and len(msg) >= 2:
                    session = self.register(ws, msg)
                    if session is None:
                        break
                    role = "host"
                elif role is None and op == 8 and len(msg) >= 2:
                    joined = self.join(ws, msg)
                    if joined is None:
                        break
                    session, cid = joined
                    role = "friend"
                elif role is None and op == 20 and len(msg) >= 18:
                    if msg[1] != 2:
                        ws.send(bytes([7, E_VERSION]))
                        break
                    ip = ws.peer[0]
                    if self.cloud_ips.get(ip, 0) >= MAX_CLOUD_PER_IP:
                        ws.send(bytes([7, E_TOO_OFTEN]))
                        break
                    self.cloud_ips[ip] = self.cloud_ips.get(ip, 0) + 1
                    try:
                        pid = player_id(bytes(msg[2:18]))
                        await ws.send_all(bytes([21]) + pid)
                        await self.cloud(ws, pid)
                    finally:
                        self.cloud_ips[ip] -= 1
                        if not self.cloud_ips[ip]:
                            del self.cloud_ips[ip]
                    break
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, asyncio.LimitOverrunError):
            pass
        finally:
            if role == "host" and session.host_ws is ws:
                session.host_ws = None      # the friends wait for him (HOST_WAIT)
                session.last_seen = time.monotonic()
                log.info("game %d: host connection lost", session.code)
                if session.v2:
                    session.set_host_state(STATE_GONE)
            elif role == "friend" and cid in session.clients and session.clients[cid][0] is ws:
                # kept CLIENT_RESUME for the friend to come back with his token
                session.detach_client(cid, ws)
            ws.close()

    async def housekeeping(self):
        last_daily = 0.0
        while True:
            await asyncio.sleep(5)
            now = time.monotonic()
            for s in list(self.sessions.values()):
                if s.host_ws is None:
                    limit = HOST_WAIT if s.v2 and s.friends_waiting() else SESSION_TIMEOUT
                    if now - s.last_seen > limit:
                        self.end_session(s, "host gone")
                        continue
                for cid, (ws, seen, _) in list(s.clients.items()):
                    if ws is None and now - seen > CLIENT_RESUME:
                        s.drop_client(cid, "left")
                    elif now - seen > CLIENT_TIMEOUT:
                        if ws is not None:
                            ws.close()
                        s.drop_client(cid, "timed out")
            if time.time() - last_daily > 86400:
                last_daily = time.time()
                self.store.expire_codes()
                self.store.expire_campaigns()
                self.new_ids_cleanup()

    def new_ids_cleanup(self):
        now = time.time()
        self.store.new_ids = {ip: t for ip, t in ((ip, [x for x in ts if now - x < 3600])
                                                   for ip, ts in self.store.new_ids.items()) if t}

    async def run(self):
        server = await asyncio.start_server(self.on_websocket, self.ws_bind, self.ws_port, limit=MAX_MESSAGE + 1024)
        log.info("relay up: websocket %s:%d, protocol 1 codes %d-%d", self.ws_bind, self.ws_port,
                 self.v1_codes[0], self.v1_codes[-1])
        async with server:
            await self.housekeeping()


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="Crysis Coop relay")
    ap.add_argument("--ws-bind", default="127.0.0.1", help="the WebSocket listener (behind Caddy)")
    ap.add_argument("--ws-port", type=int, default=64190)
    ap.add_argument("--first-port", type=int, default=64101, help="protocol 1 codes")
    ap.add_argument("--last-port", type=int, default=64164)
    ap.add_argument("--data", default=os.path.join(here, "data"), help="codes and campaigns")
    # accepted and ignored: the UDP ports of earlier versions
    ap.add_argument("--control-port", type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument("--bind", default="", help=argparse.SUPPRESS)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(Relay(args.ws_bind, args.ws_port, args.first_port, args.last_port, args.data).run())


if __name__ == "__main__":
    main()
