#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Strikers relay: internet rooms for Local Wireless.

The game's same-network transport (SLDN, runtime/services/nnldn_lan.inc) carries Local Wireless over UDP. This server
lets games that are not on the same network find each other's rooms and passes their SLDN datagrams between them.
Players need no account, port forwarding or VPN, and never learn each other's IP addresses.

One UDP port. Every datagram from a game starts with a 20-byte header, little endian:
    "SRL1", u8 type, u8 protocol (1), u16 0, u32 token, u64 secret
token and secret are 0 in HELLO and come from WELCOME afterwards. Every datagram from the server starts with
    "SRL1", u8 type, u8 0, u16 0, u32 field

  HELLO   1  build u64, room[16], nonce u64       -> WELCOME 2  field=token; secret u64, nonce u64
  HOST    3  the SLDN advert datagram (1217 bytes): list or refresh the caller's room (expires after 6 s)
  UNHOST  4
  LIST    5                                         -> ROOM 6  field=host token; that host's advert datagram
                                                       ... then LIST_END 7  field=count
  SEND    8  dst token u32, an SLDN datagram         -> DATA 9  field=src token; the datagram (to dst)
  PING   10  u64 (echoed)                            -> PONG 11  u64
  BYE    12
                                                       ERROR 13 field=1 unknown session (say HELLO again),
                                                                2 another protocol, 3 server full

build is the game's LDN build fingerprint: only games of the same release see each other. room is the first 16 bytes
of SHA-256("Strikers room key:" + key), or 16 zero bytes for the public room; the key itself never reaches the server.
Tokens are 1..0xFFFFFF: the game keeps them as IPv4 words in 0.0.0.0/8.

Everything a game sends is untrusted:
  - X may send to Y only when both have the same build and room, and Y has a room listed or Y has sent to X before;
    the server never sends to an address a game names.
  - an answer to a datagram without a valid session is never bigger than that datagram;
  - adverts are checked as the game checks them (lan_valid_info) before anyone sees them;
  - sessions, rooms and HELLOs per IP address, and bytes and datagrams per session, are limited.
Nothing is written to disk and IP addresses are never logged.
"""
import argparse
import asyncio
import hashlib
import os
import signal
import struct
import sys
import time

MAGIC = b"SRL1"
PROTOCOL = 1
C_HEADER = struct.Struct("<4sBBHIQ")  # 20 bytes
S_HEADER = struct.Struct("<4sBBHI")   # 12 bytes
HELLO, WELCOME, HOST, UNHOST, LIST, ROOM, LIST_END, SEND, DATA, PING, PONG, BYE, ERROR = range(1, 14)
ERR_SESSION, ERR_PROTOCOL, ERR_FULL = 1, 2, 3

# SLDN (runtime/services/ldn_lan.h)
SLDN_HEADER = 64
NI_SIZE = 0x480
LP_ADVERT = 2
ADVERT_SIZE = SLDN_HEADER + NI_SIZE + 1
MAX_SLDN = SLDN_HEADER + 4096
ROOM_KEY_PREFIX = b"Strikers room key:"


def room_hash(key: str) -> bytes:
    """What the game sends for a room key (relay_transport.c does the same)."""
    key = key.strip()
    return hashlib.sha256(ROOM_KEY_PREFIX + key.encode("utf-8")).digest()[:16] if key else bytes(16)


def node(i):
    return 0x68 + 0x40 * i


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def valid_info(n: bytes) -> bool:
    """lan_valid_info in runtime/services/nnldn_lan.inc."""
    if len(n) != NI_SIZE or n[0x26] > 32 or n[0x27 + n[0x26]] or not 1 <= n[0x66] <= 8 or not 1 <= n[0x67] <= n[0x66]:
        return False
    if n[0x4b] != 2 or u16(n, 0x26a) > 384 or not n[node(0) + 11]:
        return False
    base = u32(n, node(0)) & 0xFFFFFF00
    if base & 0xFFFF0000 != 0xA9FE0000:
        return False
    count = 0
    for i in range(8):
        if n[node(i) + 11]:
            if i >= n[0x66] or n[node(i) + 10] != i or u32(n, node(i)) != base | (i + 1) or 0 not in n[node(i) + 12:node(i) + 45]:
                return False
            count += 1
    return count == n[0x67]


def valid_advert(d: bytes, build: int) -> bool:
    if len(d) != ADVERT_SIZE or d[:4] != b"SLDN":
        return False
    version, kind, size, sbuild, ident = struct.unpack_from("<HHIQQ", d, 4)
    info = d[SLDN_HEADER:SLDN_HEADER + NI_SIZE]
    return (version == 1 and kind == LP_ADVERT and size == NI_SIZE + 1 and sbuild == build and ident != 0
            and d[28:44] == info[0x10:0x20] and d[-1] in (0, 1) and valid_info(info))


class Bucket:
    """Token bucket: rate per second, burst."""
    __slots__ = ("rate", "burst", "level", "at")

    def __init__(self, rate, burst, now):
        self.rate, self.burst, self.level, self.at = rate, burst, burst, now

    def take(self, amount, now):
        self.level = min(self.burst, self.level + (now - self.at) * self.rate)
        self.at = now
        if self.level < amount:
            return False
        self.level -= amount
        return True


class Session:
    __slots__ = ("token", "secret", "nonce", "addr", "build", "room", "seen", "advert", "hosted_at", "listed_at",
                 "contacts", "bytes", "packets", "lists")

    def __init__(self, token, secret, nonce, addr, build, room, now, limits):
        self.token, self.secret, self.nonce, self.addr, self.build, self.room = token, secret, nonce, addr, build, room
        self.seen = now
        self.advert = None   # the listed room's advert datagram
        self.hosted_at = 0.0  # last HOST
        self.listed_at = 0.0  # first HOST of this room (for "newest first")
        self.contacts = {}    # token -> last time, both ways
        self.bytes = Bucket(limits.bandwidth, limits.bandwidth * 2, now)
        self.packets = Bucket(limits.packets, limits.packets * 2, now)
        self.lists = Bucket(5, 10, now)


class Limits:
    def __init__(self, **kw):
        self.max_sessions = 5000
        self.sessions_per_ip = 16
        self.rooms_per_ip = 8
        self.hellos_per_ip = 5      # a second (burst 20)
        self.bandwidth = 256 * 1024  # bytes a second per session, each way
        self.packets = 600           # datagrams a second per session
        self.session_timeout = 20.0
        self.room_timeout = 6.0
        self.list_max = 24
        self.contacts_max = 32
        for k, v in kw.items():
            setattr(self, k, v)


class Relay:
    """The relay's state. handle() takes one datagram and returns the datagrams to send: [(bytes, addr)]."""

    def __init__(self, limits=None, rand=os.urandom, verbose=False):
        self.limits = limits or Limits()
        self.rand = rand
        self.verbose = verbose
        self.sessions = {}   # token -> Session
        self.by_ip = {}      # ip -> set of tokens
        self.hello = {}      # ip -> Bucket
        self.stats = {"in": 0, "out": 0, "dropped": 0, "relayed": 0}

    # ---- helpers
    def out(self, kind, field=0, body=b""):
        return S_HEADER.pack(MAGIC, kind, 0, 0, field) + body

    def new_token(self):
        while True:
            t = int.from_bytes(self.rand(3), "little")
            if t and t not in self.sessions:
                return t

    def drop(self, s: Session):
        self.sessions.pop(s.token, None)
        tokens = self.by_ip.get(s.addr[0])
        if tokens is not None:
            tokens.discard(s.token)
            if not tokens:
                del self.by_ip[s.addr[0]]
        for t in s.contacts:
            o = self.sessions.get(t)
            if o:
                o.contacts.pop(s.token, None)
        if self.verbose and s.advert:
            print(f"[relay] room closed ({self.room_name(s)})", flush=True)

    def rooms_of_ip(self, ip):
        return sum(1 for t in self.by_ip.get(ip, ()) if self.sessions[t].advert is not None)

    @staticmethod
    def room_name(s):
        return "public" if s.room == bytes(16) else "key " + s.room.hex()

    def contact(self, a: Session, b: Session, now):
        for x, y in ((a, b), (b, a)):
            if y.token not in x.contacts and len(x.contacts) >= self.limits.contacts_max:
                del x.contacts[min(x.contacts, key=x.contacts.get)]
            x.contacts[y.token] = now

    # ---- the protocol
    def handle(self, data: bytes, addr, now: float):
        self.stats["in"] += 1
        if len(data) < C_HEADER.size or data[:4] != MAGIC:
            self.stats["dropped"] += 1
            return []
        _, kind, proto, _, token, secret = C_HEADER.unpack_from(data)
        body = data[C_HEADER.size:]
        if proto != PROTOCOL:
            return [(self.out(ERROR, ERR_PROTOCOL), addr)]
        if kind == HELLO:
            return self.on_hello(body, addr, now)
        s = self.sessions.get(token)
        if s is None or s.secret != secret:
            bucket = self.hello.setdefault(addr[0], Bucket(self.limits.hellos_per_ip, 20, now))
            return [(self.out(ERROR, ERR_SESSION), addr)] if bucket.take(1, now) else []
        if s.addr != addr:  # the player's router changed its port: follow it (the secret proves it is them)
            self.by_ip.get(s.addr[0], set()).discard(s.token)
            if not self.by_ip.get(s.addr[0], True):
                self.by_ip.pop(s.addr[0], None)
            s.addr = addr
            self.by_ip.setdefault(addr[0], set()).add(s.token)
        s.seen = now
        if not s.packets.take(1, now) or not s.bytes.take(len(data), now):
            self.stats["dropped"] += 1
            return []
        if kind == HOST:
            if not valid_advert(body, s.build):
                self.stats["dropped"] += 1
                return []
            if s.advert is None:
                if self.rooms_of_ip(addr[0]) >= self.limits.rooms_per_ip:
                    return [(self.out(ERROR, ERR_FULL), addr)]
                s.listed_at = now
                if self.verbose:
                    print(f"[relay] room listed ({self.room_name(s)})", flush=True)
            s.advert, s.hosted_at = bytes(body), now
            return []
        if kind == UNHOST:
            s.advert = None
            return []
        if kind == LIST:
            if not s.lists.take(1, now):
                return []
            rooms = [o for o in self.sessions.values() if o is not s and o.advert is not None and o.build == s.build
                     and o.room == s.room]
            # rooms with a free place first, then the newest
            rooms.sort(key=lambda o: (o.advert[SLDN_HEADER + 0x67] >= o.advert[SLDN_HEADER + 0x66], -o.listed_at))
            rooms = rooms[:self.limits.list_max]
            replies = [(self.out(ROOM, o.token, o.advert), addr) for o in rooms]
            replies.append((self.out(LIST_END, len(rooms)), addr))
            return replies
        if kind == SEND:
            if len(body) < 4 + SLDN_HEADER or len(body) > 4 + MAX_SLDN:
                self.stats["dropped"] += 1
                return []
            d = self.sessions.get(u32(body, 0))
            if (d is None or d is s or d.build != s.build or d.room != s.room
                    or (d.advert is None and d.token not in s.contacts)):
                self.stats["dropped"] += 1
                return []
            if not d.bytes.take(len(body), now):
                self.stats["dropped"] += 1
                return []
            self.contact(s, d, now)
            self.stats["relayed"] += 1
            return [(self.out(DATA, s.token, body[4:]), d.addr)]
        if kind == PING:
            return [(self.out(PONG, 0, body[:8]), addr)] if len(body) >= 8 else []
        if kind == BYE:
            self.drop(s)
            return []
        self.stats["dropped"] += 1
        return []

    def on_hello(self, body, addr, now):
        if len(body) < 32:
            return []
        build, room, nonce = struct.unpack_from("<Q16sQ", body)
        ip = addr[0]
        for t in self.by_ip.get(ip, ()):  # a resent HELLO: the same session
            s = self.sessions[t]
            if s.addr == addr and s.nonce == nonce and s.build == build and s.room == room:
                s.seen = now
                return [(self.out(WELCOME, s.token, struct.pack("<QQ", s.secret, nonce)), addr)]
        bucket = self.hello.setdefault(ip, Bucket(self.limits.hellos_per_ip, 20, now))
        if not bucket.take(1, now):
            return []
        if len(self.sessions) >= self.limits.max_sessions or len(self.by_ip.get(ip, ())) >= self.limits.sessions_per_ip:
            return [(self.out(ERROR, ERR_FULL), addr)]
        token = self.new_token()
        secret = int.from_bytes(self.rand(8), "little") or 1
        self.sessions[token] = Session(token, secret, nonce, addr, build, bytes(room), now, self.limits)
        self.by_ip.setdefault(ip, set()).add(token)
        return [(self.out(WELCOME, token, struct.pack("<QQ", secret, nonce)), addr)]

    def expire(self, now):
        for s in list(self.sessions.values()):
            if now - s.seen > self.limits.session_timeout:
                self.drop(s)
            elif s.advert is not None and now - s.hosted_at > self.limits.room_timeout:
                s.advert = None
        for ip in [ip for ip, b in self.hello.items() if now - b.at > 60]:
            del self.hello[ip]


class Protocol(asyncio.DatagramProtocol):
    def __init__(self, relay):
        self.relay = relay
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        for reply, to in self.relay.handle(data, addr, time.monotonic()):
            self.transport.sendto(reply, to)
            self.relay.stats["out"] += 1

    def error_received(self, exc):
        pass  # ICMP port unreachable from a game that closed: nothing to do


async def serve(args):
    limits = Limits(max_sessions=args.max_sessions, sessions_per_ip=args.sessions_per_ip,
                    rooms_per_ip=args.rooms_per_ip, bandwidth=args.bandwidth)
    relay = Relay(limits, verbose=args.verbose)
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: Protocol(relay), local_addr=(args.addr, args.port))
    print(f"[relay] listening on udp {args.addr}:{args.port}", flush=True)
    stop = asyncio.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop.set)
        except (NotImplementedError, RuntimeError):
            pass  # Windows: Ctrl+C ends asyncio.run
    last = 0.0
    try:
        while not stop.is_set():
            try:
                await asyncio.wait_for(stop.wait(), 1.0)
            except asyncio.TimeoutError:
                pass
            now = time.monotonic()
            relay.expire(now)
            if args.verbose and now - last >= 60:
                last = now
                rooms = sum(1 for s in relay.sessions.values() if s.advert is not None)
                print(f"[relay] sessions={len(relay.sessions)} rooms={rooms} {relay.stats}", flush=True)
    finally:
        transport.close()


def main(argv=None):
    p = argparse.ArgumentParser(description="Strikers relay: internet rooms for Local Wireless")
    p.add_argument("--addr", default="0.0.0.0")
    p.add_argument("--port", type=int, default=24890)
    p.add_argument("--max-sessions", type=int, default=5000)
    p.add_argument("--sessions-per-ip", type=int, default=16)
    p.add_argument("--rooms-per-ip", type=int, default=8)
    p.add_argument("--bandwidth", type=int, default=256 * 1024, help="bytes a second per player, each way")
    p.add_argument("-v", "--verbose", action="store_true", help="log rooms and counts (never IP addresses)")
    args = p.parse_args(argv)
    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
