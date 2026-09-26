#!/usr/bin/env python3
"""Crysis Coop relay.

The game itself runs on the host's PC (AI, physics, story). This service only
forwards the game's UDP datagrams between the host and his friends, so nobody
has to open or forward ports.

Coop.dll talks to it over a WebSocket (wss://..., behind Caddy on port 443,
which passes through practically any firewall):

  friend's game --UDP--> Coop.dll (127.0.0.1) ==wss==> relay ==wss==> Coop.dll
                                                                   --UDP--> host's game (127.0.0.1:64087)

Every coop host gets a session code; friends join with "coop_join <code>".

WebSocket messages (binary, integers little endian):
  host   -> relay  1 REGISTER   ver:u8 prev_code:u16 prev_secret:8
  relay  -> host   2 REGISTERED code:u16 secret:8
  both   -> relay  3 PING       (answered with 3)
  relay  -> host   4 DATA       client:u16 payload
  host   -> relay  5 DATA       client:u16 payload
  relay  -> host   6 CLOSE      client:u16            (friend gone)
  relay  -> any    7 ERROR      code:u8   (1 relay full, 2 version, 3 no such game, 4 game full)
  friend -> relay  8 JOIN       ver:u8 code:u16 token:8   (the same token: back as the same client within 20 s)
  relay  -> friend 9 JOINED
  friend -> relay 10 DATA       payload
  relay  -> friend 11 DATA      payload
  friend -> relay 12 CANDIDATES token:8 count:u8 (ip:4 port:u16)*
  relay  -> host  13 CANDIDATES client:u16 token:8 count:u8 (ip:4 port:u16)*
  host   -> relay 14 CANDIDATES client:u16 count:u8 (ip:4 port:u16)*
  relay  -> friend 15 CANDIDATES count:u8 (ip:4 port:u16)*

Direct path: the relay is also where the host and a friend swap the UDP
addresses they can be reached at (their local ones and the public one a STUN
server reported). Both then send UDP to each other (NAT hole punching); when
that gets through, the game traffic goes straight between them and the relay
stays as the fallback. PING (3) is echoed as it came (timestamps).

A friend may also send plain UDP to the session's public port (the code is
that port number) when the provider's firewall lets UDP in; the host may
likewise tunnel over UDP (see the UDP control protocol below). Both are
optional: the WebSocket path alone is enough.

UDP control port (all packets start with 'CC' op):
  1 REGISTER ver:u8 prev_key:8 prev_port:u16 / 2 REGISTERED key:8 port:u16 /
  3 KEEPALIVE key:8 / 4 DATA client:u16 payload (to host) /
  5 DATA key:8 client:u16 payload (from host) / 6 CLOSE client:u16 / 7 ERROR code:u8
"""

import argparse
import asyncio
import base64
import hashlib
import logging
import os
import struct
import time

PROTOCOL_VERSION = 1
MAGIC = b"CC"

SESSION_TIMEOUT = 45.0      # s without the host: the session ends, its code is freed
CLIENT_TIMEOUT = 60.0       # s without a packet from a friend: forgotten
CLIENT_RESUME = 20.0        # s a friend whose connection broke may come back as the same client
MAX_CLIENTS = 8             # friends per session
MAX_PAYLOAD = 1500
MAX_SESSIONS_PER_IP = 4     # hosts behind one address
RATE_LIMIT = 2_000_000      # bytes/s per session and direction (a coop game needs ~50 KB/s)
WS_BACKLOG_LIMIT = 1 << 20  # bytes queued to a slow WebSocket before datagrams are dropped

log = logging.getLogger("coop_relay")


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
            if length > 65536:
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

    def close(self):
        if not self.closed:
            self._frame(0x8, b"")
            self.closed = True
            self.writer.close()


# --------------------------------------------------------------------------
class Session:
    def __init__(self, relay, code, host_ip):
        self.relay = relay
        self.code = code
        self.secret = os.urandom(8)
        self.host_ip = host_ip
        self.host_ws = None         # WebSocket of the host
        self.host_udp = None        # or the address of a UDP host
        self.udp_key = None
        self.last_seen = time.monotonic()
        self.transport = None       # the public UDP port (optional path)
        self.clients = {}           # client id -> [address or WebSocket, last seen]
        self.by_peer = {}           # address / WebSocket -> client id
        self.tokens = {}            # a WebSocket friend's token -> client id (to come back)
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

    def has_host(self):
        return self.host_ws is not None or self.host_udp is not None

    def client_id(self, peer, label):
        cid = self.by_peer.get(peer)
        if cid is not None:
            self.clients[cid][1] = time.monotonic()
            return cid
        if len(self.clients) >= MAX_CLIENTS:
            return None
        cid = self.next_id
        self.next_id = self.next_id % 65535 + 1
        self.clients[cid] = [peer, time.monotonic()]
        self.by_peer[peer] = cid
        log.info("game %d: friend %s joined as client %d", self.code, label, cid)
        return cid

    def resume_client(self, token, ws):
        cid = self.tokens.get(token)
        entry = self.clients.get(cid)
        if not entry:
            return None
        old = entry[0]
        if isinstance(old, WebSocket):
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
            self.to_host(bytes([6]) + struct.pack("<H", cid), MAGIC + bytes([6]) + struct.pack("<H", cid))

    def to_host(self, ws_message, udp_packet):
        if self.host_ws:
            self.host_ws.send(ws_message)
        elif self.host_udp:
            self.relay.send_control(udp_packet, self.host_udp)

    def from_friend(self, cid, data):
        """A datagram of a friend's game, for the host's game."""
        self.refill()
        if self.budget_in < len(data) or len(data) > MAX_PAYLOAD:
            return
        self.budget_in -= len(data)
        self.bytes_in += len(data)
        head = struct.pack("<H", cid)
        self.to_host(bytes([4]) + head + data, MAGIC + bytes([4]) + head + data)

    def from_host(self, cid, data):
        """A datagram of the host's game, for one friend."""
        entry = self.clients.get(cid)
        self.refill()
        if not entry or self.budget_out < len(data):
            return
        self.budget_out -= len(data)
        self.bytes_out += len(data)
        peer = entry[0]
        if peer is None:
            return          # the friend's connection broke, he may come back
        if isinstance(peer, WebSocket):
            peer.send(bytes([11]) + data)
        elif self.transport:
            self.transport.sendto(data, peer)


class PublicPort(asyncio.DatagramProtocol):
    """Optional: friends sending plain UDP to the session port."""

    def __init__(self, session):
        self.session = session

    def connection_made(self, transport):
        self.session.transport = transport

    def datagram_received(self, data, addr):
        cid = self.session.client_id(addr, "%s:%d (udp)" % addr)
        if cid is not None:
            self.session.from_friend(cid, data)


class Control(asyncio.DatagramProtocol):
    """Optional: hosts tunnelling over UDP."""

    def __init__(self, relay):
        self.relay = relay

    def connection_made(self, transport):
        self.relay.control = transport

    def datagram_received(self, data, addr):
        if len(data) < 3 or data[:2] != MAGIC:
            return
        op = data[2]
        if op == 5 and len(data) >= 13:
            s = self.relay.udp_session(data[3:11], addr)
            if s:
                s.from_host(struct.unpack_from("<H", data, 11)[0], data[13:])
        elif op == 3 and len(data) >= 11:
            if self.relay.udp_session(data[3:11], addr):
                self.relay.send_control(data[:11], addr)
        elif op == 1 and len(data) >= 14:
            asyncio.ensure_future(self.relay.register_udp(data, addr))


class Relay:
    def __init__(self, bind, control_port, first_port, last_port, ws_bind, ws_port):
        self.bind = bind
        self.control_port = control_port
        self.codes = list(range(first_port, last_port + 1))
        self.ws_bind = ws_bind
        self.ws_port = ws_port
        self.sessions = {}      # code -> Session
        self.control = None

    def send_control(self, data, addr):
        if self.control:
            self.control.sendto(data, addr)

    async def new_session(self, host_ip, prev_code):
        if sum(1 for s in self.sessions.values() if s.host_ip == host_ip) >= MAX_SESSIONS_PER_IP:
            log.warning("too many games from %s", host_ip)
            return None
        free = [c for c in self.codes if c not in self.sessions]
        if not free:
            log.warning("relay full")
            return None
        code = prev_code if prev_code in free else free[0]
        s = Session(self, code, host_ip)
        try:
            await asyncio.get_running_loop().create_datagram_endpoint(
                lambda: PublicPort(s), local_addr=(self.bind, code))
        except OSError as e:
            log.warning("udp port %d not available (%s): websocket friends only", code, e)
        self.sessions[code] = s
        log.info("game %d opened for host %s", code, host_ip)
        return s

    def end_session(self, s, why):
        log.info("game %d closed (%s), %d bytes to the host, %d to friends", s.code, why, s.bytes_in, s.bytes_out)
        self.sessions.pop(s.code, None)
        if s.transport:
            s.transport.close()
        for peer, _ in list(s.clients.values()):
            if isinstance(peer, WebSocket):
                peer.close()

    # ---- UDP hosts
    def udp_session(self, key, addr):
        for s in self.sessions.values():
            if s.udp_key == bytes(key):
                s.last_seen = time.monotonic()
                s.host_udp = addr
                return s
        return None

    async def register_udp(self, data, addr):
        if data[3] != PROTOCOL_VERSION:
            self.send_control(MAGIC + bytes([7, 2]), addr)
            return
        prev_key = bytes(data[4:12])
        prev_port = struct.unpack_from("<H", data, 12)[0]
        s = self.sessions.get(prev_port)
        if not (s and s.udp_key == prev_key and prev_key != bytes(8)):
            s = next((x for x in self.sessions.values() if x.host_udp == addr), None)
        if s is None:
            s = await self.new_session(addr[0], prev_port)
            if s is None:
                self.send_control(MAGIC + bytes([7, 1]), addr)
                return
            s.udp_key = os.urandom(8)
        s.host_udp = addr
        s.last_seen = time.monotonic()
        self.send_control(MAGIC + bytes([2]) + s.udp_key + struct.pack("<H", s.code), addr)

    # ---- WebSocket players (hosts and friends)
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
                    if session and role == "host":
                        session.last_seen = time.monotonic()
                    elif session and role == "friend" and cid in session.clients:
                        # on a direct path the friend sends no data here, only pings
                        session.clients[cid][1] = time.monotonic()
                elif role == "host" and op == 5 and len(msg) >= 3:
                    session.last_seen = time.monotonic()
                    session.from_host(struct.unpack_from("<H", msg, 1)[0], msg[3:])
                elif role == "friend" and op == 12 and len(msg) >= 10:
                    if session.code in self.sessions and session.host_ws:
                        session.host_ws.send(bytes([13]) + struct.pack("<H", cid) + msg[1:64])
                elif role == "host" and op == 14 and len(msg) >= 4:
                    entry = session.clients.get(struct.unpack_from("<H", msg, 1)[0])
                    if entry and isinstance(entry[0], WebSocket):
                        entry[0].send(bytes([15]) + msg[3:64])
                elif role == "friend" and op == 10:
                    if session.code in self.sessions and session.has_host():
                        session.clients[cid][1] = time.monotonic()
                        session.from_friend(cid, msg[1:])
                elif role is None and op == 1 and len(msg) >= 12:
                    if msg[1] != PROTOCOL_VERSION:
                        ws.send(bytes([7, 2]))
                        break
                    prev_code = struct.unpack_from("<H", msg, 2)[0]
                    prev = self.sessions.get(prev_code)
                    if prev and prev.secret == msg[4:12] and prev.host_ws is None:
                        session = prev      # the host is back (lost connection)
                        log.info("game %d: host back", prev_code)
                    else:
                        session = await self.new_session(ws.peer[0], prev_code)
                    if session is None:
                        ws.send(bytes([7, 1]))
                        break
                    role = "host"
                    session.host_ws = ws
                    session.last_seen = time.monotonic()
                    ws.send(bytes([2]) + struct.pack("<H", session.code) + session.secret)
                elif role is None and op == 8 and len(msg) >= 4:
                    if msg[1] != PROTOCOL_VERSION:
                        ws.send(bytes([7, 2]))
                        break
                    code = struct.unpack_from("<H", msg, 2)[0]
                    session = self.sessions.get(code)
                    if not session or not session.has_host():
                        ws.send(bytes([7, 3]))
                        break
                    token = bytes(msg[4:12]) if len(msg) >= 12 else None
                    cid = session.resume_client(token, ws) if token else None
                    if cid is not None:
                        log.info("game %d: client %d back", session.code, cid)
                    else:
                        cid = session.client_id(ws, "%s (wss)" % ws.peer[0])
                        if cid is None:
                            ws.send(bytes([7, 4]))
                            break
                        if token:
                            session.tokens[token] = cid
                    role = "friend"
                    ws.send(b"\x09")
        except (asyncio.IncompleteReadError, ConnectionError, asyncio.TimeoutError, asyncio.LimitOverrunError):
            pass
        finally:
            if role == "host" and session and session.host_ws is ws:
                session.host_ws = None      # kept SESSION_TIMEOUT for the host to come back
                session.last_seen = time.monotonic()
                log.info("game %d: host connection lost", session.code)
            elif role == "friend" and session and cid in session.clients and session.clients[cid][0] is ws:
                # kept CLIENT_RESUME for the friend to come back with his token
                session.detach_client(cid, ws)
            ws.close()

    async def housekeeping(self):
        while True:
            await asyncio.sleep(5)
            now = time.monotonic()
            for s in list(self.sessions.values()):
                if s.host_ws is None and now - s.last_seen > SESSION_TIMEOUT:
                    self.end_session(s, "host gone")
                    continue
                for cid, (peer, seen) in list(s.clients.items()):
                    if peer is None and now - seen > CLIENT_RESUME:
                        s.drop_client(cid, "left")
                    elif now - seen > CLIENT_TIMEOUT:
                        if isinstance(peer, WebSocket):
                            peer.close()
                        s.drop_client(cid, "timed out")

    async def run(self):
        loop = asyncio.get_running_loop()
        await loop.create_datagram_endpoint(lambda: Control(self), local_addr=(self.bind, self.control_port))
        server = await asyncio.start_server(self.on_websocket, self.ws_bind, self.ws_port)
        log.info("relay up: websocket %s:%d, udp control %s:%d, codes %d-%d", self.ws_bind, self.ws_port,
                 self.bind, self.control_port, self.codes[0], self.codes[-1])
        async with server:
            await self.housekeeping()


def main():
    ap = argparse.ArgumentParser(description="Crysis Coop relay")
    ap.add_argument("--bind", default="0.0.0.0", help="address of the optional UDP ports")
    ap.add_argument("--control-port", type=int, default=64100)
    ap.add_argument("--first-port", type=int, default=64101)
    ap.add_argument("--last-port", type=int, default=64164)
    ap.add_argument("--ws-bind", default="127.0.0.1", help="the WebSocket listener (behind Caddy)")
    ap.add_argument("--ws-port", type=int, default=64190)
    args = ap.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    asyncio.run(Relay(args.bind, args.control_port, args.first_port, args.last_port,
                      args.ws_bind, args.ws_port).run())


if __name__ == "__main__":
    main()
