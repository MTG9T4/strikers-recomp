# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for the Strikers relay's protocol logic (no sockets). Run: python -m unittest -v test_strikers_relay"""
import os
import random
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strikers_relay as R  # noqa: E402

BUILD = 0x2E90F0815B1F92A1


def advert(build=BUILD, ident=0x1111, players=1, max_players=4, session=b"S" * 16, private=0):
    n = bytearray(R.NI_SIZE)
    n[0x10:0x20] = session
    n[0x26] = 32
    n[0x27:0x47] = session.hex().encode()
    n[0x4B] = 2
    n[0x66] = max_players
    n[0x67] = players
    base = 0xA9FE4200
    for i in range(players):
        o = R.node(i)
        struct.pack_into("<I", n, o, base | (i + 1))
        n[o + 10] = i
        n[o + 11] = 1
        n[o + 12:o + 17] = b"name\0"
    h = bytearray(R.SLDN_HEADER)
    h[:4] = b"SLDN"
    struct.pack_into("<HHIQQ", h, 4, 1, R.LP_ADVERT, R.NI_SIZE + 1, build, ident)
    h[28:44] = session
    return bytes(h) + bytes(n) + bytes([private])


def sldn(size=80):
    d = bytearray(R.SLDN_HEADER + size)
    d[:4] = b"SLDN"
    return bytes(d)


class Client:
    """A game, as the relay sees it."""

    def __init__(self, relay, addr, build=BUILD, room=bytes(16), now=0.0, nonce=None):
        self.relay, self.addr = relay, addr
        self.nonce = nonce if nonce is not None else random.getrandbits(64)
        out = relay.handle(self.packet(R.HELLO, struct.pack("<Q16sQ", build, room, self.nonce), 0, 0), addr, now)
        assert len(out) == 1 and out[0][1] == addr, out
        _, kind, _, _, field = R.S_HEADER.unpack_from(out[0][0])
        self.kind = kind
        if kind == R.WELCOME:
            self.token = field
            self.secret, echoed = struct.unpack_from("<QQ", out[0][0], R.S_HEADER.size)
            assert echoed == self.nonce

    @staticmethod
    def packet(kind, body=b"", token=0, secret=0, proto=R.PROTOCOL):
        return R.C_HEADER.pack(R.MAGIC, kind, proto, 0, token, secret) + body

    def send(self, kind, body=b"", now=0.0):
        return self.relay.handle(self.packet(kind, body, self.token, self.secret), self.addr, now)


def kinds(out):
    return [R.S_HEADER.unpack_from(d)[1] for d, _ in out]


class RelayTest(unittest.TestCase):
    def setUp(self):
        random.seed(5)
        self.relay = R.Relay(rand=lambda n: random.getrandbits(8 * n).to_bytes(n, "little"))

    def test_room_hash(self):
        self.assertEqual(R.room_hash(""), bytes(16))
        self.assertEqual(R.room_hash("  abc "), R.room_hash("abc"))
        self.assertNotEqual(R.room_hash("abc"), R.room_hash("abd"))

    def test_advert_validation(self):
        self.assertTrue(R.valid_advert(advert(), BUILD))
        self.assertFalse(R.valid_advert(advert(build=5), BUILD))
        self.assertFalse(R.valid_advert(advert()[:-1], BUILD))
        bad = bytearray(advert())
        bad[R.SLDN_HEADER + 0x67] = 2  # says 2 players, lists 1
        self.assertFalse(R.valid_advert(bytes(bad), BUILD))
        bad = bytearray(advert())
        bad[30] ^= 1  # header session differs from the info's
        self.assertFalse(R.valid_advert(bytes(bad), BUILD))
        bad = bytearray(advert())
        struct.pack_into("<I", bad, R.SLDN_HEADER + R.node(0), 0x0A000001)  # not 169.254
        self.assertFalse(R.valid_advert(bytes(bad), BUILD))

    def test_host_list_join_and_relay(self):
        host = Client(self.relay, ("1.1.1.1", 1000))
        guest = Client(self.relay, ("2.2.2.2", 2000))
        self.assertEqual(host.send(R.HOST, advert()), [])
        out = guest.send(R.LIST)
        self.assertEqual(kinds(out), [R.ROOM, R.LIST_END])
        self.assertEqual(R.S_HEADER.unpack_from(out[0][0])[4], host.token)
        self.assertEqual(out[0][0][R.S_HEADER.size:], advert())
        # guest -> host (host has a room), then host -> guest (guest has sent to host)
        out = guest.send(R.SEND, struct.pack("<I", host.token) + sldn())
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0][1], host.addr)
        self.assertEqual(R.S_HEADER.unpack_from(out[0][0])[4], guest.token)
        self.assertEqual(out[0][0][R.S_HEADER.size:], sldn())
        out = host.send(R.SEND, struct.pack("<I", guest.token) + sldn(100))
        self.assertEqual([to for _, to in out], [guest.addr])

    def test_no_sending_to_strangers(self):
        a = Client(self.relay, ("1.1.1.1", 1000))
        b = Client(self.relay, ("2.2.2.2", 2000))
        self.assertEqual(a.send(R.SEND, struct.pack("<I", b.token) + sldn()), [])  # b has no room, never sent to a
        self.assertEqual(a.send(R.SEND, struct.pack("<I", a.token) + sldn()), [])  # self
        self.assertEqual(a.send(R.SEND, struct.pack("<I", 0xABCDEF) + sldn()), [])  # nobody

    def test_room_key_and_build_isolation(self):
        host = Client(self.relay, ("1.1.1.1", 1000), room=R.room_hash("friends"))
        host.send(R.HOST, advert())
        public = Client(self.relay, ("2.2.2.2", 2000))
        friend = Client(self.relay, ("3.3.3.3", 3000), room=R.room_hash("friends"))
        other_build = Client(self.relay, ("4.4.4.4", 4000), build=BUILD + 1, room=R.room_hash("friends"))
        self.assertEqual(kinds(public.send(R.LIST)), [R.LIST_END])
        self.assertEqual(kinds(friend.send(R.LIST)), [R.ROOM, R.LIST_END])
        self.assertEqual(kinds(other_build.send(R.LIST)), [R.LIST_END])
        self.assertEqual(public.send(R.SEND, struct.pack("<I", host.token) + sldn()), [])
        self.assertEqual(other_build.send(R.SEND, struct.pack("<I", host.token) + sldn()), [])
        self.assertEqual(len(friend.send(R.SEND, struct.pack("<I", host.token) + sldn())), 1)

    def test_host_advert_must_match_session_build(self):
        host = Client(self.relay, ("1.1.1.1", 1000))
        host.send(R.HOST, advert(build=BUILD + 1))
        guest = Client(self.relay, ("2.2.2.2", 2000))
        self.assertEqual(kinds(guest.send(R.LIST)), [R.LIST_END])

    def test_expiry(self):
        host = Client(self.relay, ("1.1.1.1", 1000))
        host.send(R.HOST, advert(), now=0)
        guest = Client(self.relay, ("2.2.2.2", 2000))
        self.relay.expire(5.0)
        self.assertEqual(kinds(guest.send(R.LIST, now=5.0)), [R.ROOM, R.LIST_END])
        self.relay.expire(7.0)  # no HOST for 7 s: the room is gone, the session stays
        self.assertEqual(kinds(guest.send(R.LIST, now=7.0)), [R.LIST_END])
        self.assertIn(host.token, self.relay.sessions)
        self.assertEqual(kinds(guest.send(R.PING, b"12345678", now=25.0)), [R.PONG])
        self.relay.expire(30.0)  # nothing from host for 30 s; guest was heard at 25 s
        self.assertNotIn(host.token, self.relay.sessions)
        self.assertIn(guest.token, self.relay.sessions)

    def test_unhost_and_bye(self):
        host = Client(self.relay, ("1.1.1.1", 1000))
        host.send(R.HOST, advert())
        guest = Client(self.relay, ("2.2.2.2", 2000))
        host.send(R.UNHOST)
        self.assertEqual(kinds(guest.send(R.LIST)), [R.LIST_END])
        host.send(R.BYE)
        self.assertNotIn(host.token, self.relay.sessions)
        self.assertEqual(kinds(host.send(R.PING, b"12345678")), [R.ERROR])

    def test_list_order_and_limit(self):
        self.relay.limits.list_max = 3
        hosts = []
        for i in range(5):
            h = Client(self.relay, (f"10.0.0.{i}", 1000))
            full = i == 4
            h.send(R.HOST, advert(ident=100 + i, players=2 if full else 1, max_players=2, session=bytes([i]) * 16),
                   now=float(i))
            hosts.append(h)
        guest = Client(self.relay, ("2.2.2.2", 2000))
        out = guest.send(R.LIST, now=5.0)
        tokens = [R.S_HEADER.unpack_from(d)[4] for d, _ in out[:-1]]
        self.assertEqual(tokens, [hosts[3].token, hosts[2].token, hosts[1].token])  # free first, newest first

    def test_resent_hello_same_session(self):
        a = Client(self.relay, ("1.1.1.1", 1000), nonce=42)
        b = Client(self.relay, ("1.1.1.1", 1000), nonce=42)
        self.assertEqual((a.token, a.secret), (b.token, b.secret))
        c = Client(self.relay, ("1.1.1.1", 1000), nonce=43)
        self.assertNotEqual(a.token, c.token)

    def test_nat_rebinding_follows_secret(self):
        host = Client(self.relay, ("1.1.1.1", 1000))
        host.send(R.HOST, advert())
        guest = Client(self.relay, ("2.2.2.2", 2000))
        guest.addr = ("2.2.2.2", 2001)
        out = guest.send(R.SEND, struct.pack("<I", host.token) + sldn())
        self.assertEqual(len(out), 1)
        out = host.send(R.SEND, struct.pack("<I", guest.token) + sldn())
        self.assertEqual(out[0][1], ("2.2.2.2", 2001))

    def test_wrong_secret_and_protocol(self):
        a = Client(self.relay, ("1.1.1.1", 1000))
        out = self.relay.handle(Client.packet(R.LIST, b"", a.token, a.secret ^ 1), ("9.9.9.9", 9), 0.0)
        self.assertEqual(kinds(out), [R.ERROR])
        self.assertLessEqual(len(out[0][0]), R.C_HEADER.size)
        out = self.relay.handle(Client.packet(R.HELLO, bytes(32), proto=9), ("9.9.9.9", 9), 0.0)
        self.assertEqual(kinds(out), [R.ERROR])
        self.assertEqual(R.S_HEADER.unpack_from(out[0][0])[4], R.ERR_PROTOCOL)

    def test_limits(self):
        self.relay.limits.sessions_per_ip = 2
        Client(self.relay, ("1.1.1.1", 1))
        Client(self.relay, ("1.1.1.1", 2))
        self.assertEqual(Client(self.relay, ("1.1.1.1", 3)).kind, R.ERROR)
        self.relay.limits.rooms_per_ip = 1
        a = Client(self.relay, ("5.5.5.5", 1))
        b = Client(self.relay, ("5.5.5.5", 2))
        self.assertEqual(a.send(R.HOST, advert(ident=1)), [])
        self.assertEqual(kinds(b.send(R.HOST, advert(ident=2))), [R.ERROR])
        # HELLO flood from one address: answered at most the burst
        answered = sum(len(self.relay.handle(Client.packet(R.HELLO, struct.pack("<Q16sQ", BUILD, bytes(16), i)),
                                             ("6.6.6.6", i), 0.0)) for i in range(100))
        self.assertLessEqual(answered, 20)

    def test_bandwidth(self):
        self.relay.limits.bandwidth = 10000
        host = Client(self.relay, ("1.1.1.1", 1000))
        host.send(R.HOST, advert())
        guest = Client(self.relay, ("2.2.2.2", 2000))
        sent = sum(len(guest.send(R.SEND, struct.pack("<I", host.token) + sldn(1000), now=0.0)) for _ in range(100))
        self.assertLess(sent, 30)

    def test_garbage(self):
        rng = random.Random(1)
        host = Client(self.relay, ("1.1.1.1", 1000))
        for _ in range(3000):
            n = rng.randrange(0, 1400)
            junk = bytes(rng.getrandbits(8) for _ in range(n))
            if rng.random() < 0.5:  # well-formed header, random body and type
                junk = Client.packet(rng.randrange(0, 20), junk, host.token, host.secret)
            for reply, to in self.relay.handle(junk, ("7.7.7.7", 7), 1.0):
                self.assertTrue(reply.startswith(R.MAGIC))


if __name__ == "__main__":
    unittest.main()
