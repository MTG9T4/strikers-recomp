"""Portable NEX primitive, local account and UDP-probe robustness tests.

No game match is simulated or certified here. All sockets are owned loopback
endpoints. Non-loopback configuration is rejected before a socket is opened.
"""
import argparse
from dataclasses import replace
import json
from pathlib import Path
import random
import socket
import struct
import subprocess
import sys
import time
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from server.account_stub import LocalAccountProvider
from server.nex.wire import Packet, LitePacket, decode_datagram, decode_lite, decode_request, Request, response, RC4, ReceiveWindow, Fragments, inflate_bounded
from server.nex.websocket_probe import frame, read_frame
from server.nex.__main__ import load_config


class WireTests(unittest.TestCase):
    def test_independent_v1_header(self):
        # Handwritten from the offset table read at main+77474/+778c4.
        data = bytes.fromhex('ead0 01 03 0300 af a1 6200 01 00 ffff') + bytes(range(16)) + b'\x02\x01\x00abc'
        p, = decode_datagram(data)
        self.assertEqual((p.source, p.destination, p.flags, p.session, p.channel, p.sequence), (175, 161, 98, 1, 0, 65535))
        self.assertEqual((p.signature, p.options, p.payload), (bytes(range(16)), ((2, b'\0'),), b'abc'))
        self.assertEqual(p.encode(), data)
        self.assertEqual(len(decode_datagram(data+data)), 2)

    def test_independent_lite_header(self):
        data = bytes.fromhex('80 03 0300 aa 01 02 03 6200 ffff 020100') + b'abc'
        p, = decode_lite(data)
        self.assertEqual((p.source, p.destination, p.session, p.flags, p.sequence), (1, 2, 3, 98, 65535))
        self.assertEqual((p.options, p.payload, p.encode()), (((2, b'\0'),), b'abc', data))

    def test_reject_inconsistent_lengths(self):
        p = Packet(175, 161, 98, 1, 0, 1, options=((2, b'\0'),), payload=b'x').encode()
        for bad in (b'', p[:29], p[:-1], b'\0'+p[1:], p[:3]+b'\xff'+p[4:], p[:30]+b'\x02\xff'+p[32:]):
            with self.assertRaises(ValueError):
                decode_datagram(bad)
        with self.assertRaises(ValueError):
            decode_datagram(p*65)
        with self.assertRaises(ValueError):
            decode_datagram(Packet(1, 2, 2, 0, 0, 1, options=((2, b'\0'), (2, b'\1'))).encode())

    def test_rc4_published_vector_and_continuity(self):
        self.assertEqual(RC4(b'Key').apply(b'Plaintext').hex(), 'bbf316e8d940af0ad3')
        first = RC4(b'Key');self.assertEqual(first.apply(b'Plain')+first.apply(b'text'), RC4(b'Key').apply(b'Plaintext'))

    def test_window_duplicates_reorder_wrap(self):
        w = ReceiveWindow(65535)
        p = Packet(1, 2, 98, 0, 0, 0, payload=b'b')
        self.assertEqual(w.accept(p), [])
        a = replace(p, sequence=65535, payload=b'a')
        self.assertEqual(w.accept(a), [a, p])
        self.assertEqual(w.accept(a), [])
        with self.assertRaises(ValueError):
            w.accept(replace(p, sequence=100))
        other = ReceiveWindow(1)
        self.assertEqual(other.sequence, 1)

    def test_conflicting_duplicate(self):
        w = ReceiveWindow();p = Packet(1, 2, 98, 0, 0, 2)
        w.accept(p)
        with self.assertRaises(ValueError):
            w.accept(replace(p, payload=b'conflict'))

    def test_fragments_and_clear_on_error(self):
        f = Fragments();self.assertIsNone(f.append(1, b'a'));self.assertEqual(f.append(0, b'b'), b'ab')
        with self.assertRaises(ValueError):
            f.append(2, b'bad')
        self.assertEqual(f.append(0, b'fresh'), b'fresh')

    def test_deflate_bomb_truncation_trailing(self):
        data = bytes(range(256))*100
        self.assertEqual(inflate_bounded(zlib.compress(data)), data)
        for bad in (zlib.compress(bytes(300000)), zlib.compress(b'abc')[:-1], zlib.compress(b'abc')+b'extra'):
            with self.assertRaises(ValueError):
                inflate_bounded(bad)

    def test_rmc_standard_and_extended(self):
        req = struct.pack('<I', 12)+b'\x8a'+struct.pack('<II', 42, 6)+b'abc'
        self.assertEqual(decode_request(req), Request(10, 42, 6, b'abc'))
        req = struct.pack('<I', 11)+b'\xff'+struct.pack('<HII', 300, 1, 7)
        self.assertEqual(decode_request(req).protocol, 300)
        self.assertEqual(response(Request(10, 42, 6, b''), error=0x80010002), bytes.fromhex('0a000000 0a00 02000180 2a000000'))
        with self.assertRaises(ValueError):
            decode_request(req+b'x')

    def test_parser_fuzz(self):
        rng = random.Random(17)
        for _ in range(20000):
            payload = rng.randbytes(rng.randrange(400))
            for decode in (decode_datagram, decode_lite, decode_request):
                try:
                    decode(payload)
                except ValueError:
                    pass

    def test_token_rejects_tampering_wrong_issuer_key_expiry(self):
        p = LocalAccountProvider(bytes(range(32)));token = p.issue(1701, 10, now=100)
        self.assertEqual(p.verify(token, now=100), 1701)
        for broken, now, provider in ((token, 99, p), (token, 110, p), (token, 101, LocalAccountProvider(bytes(32))),
                                      (token.replace('.', '..', 1), 100, p), (token[:-3]+'abc', 100, p), ('x'*4000, 100, p)):
            with self.assertRaises(ValueError):
                provider.verify(broken, now=now)

    def test_websocket_masks_and_control_bounds(self):
        class Input:
            def __init__(self, data):
                self.data=data
            def recv(self, n):
                part,self.data=self.data[:n],self.data[n:]
                return part
        # Mask 01 02 03 04, plaintext 'test'.
        self.assertEqual(read_frame(Input(bytes.fromhex('8284 01020304 75677070'))), (True, 2, b'test'))
        for bad in (b'\x82\x01x', b'\x09\x80'+bytes(4), b'\x82\xfe\x00\x01'+bytes(4)+b'x'):
            with self.assertRaises(ValueError):
                read_frame(Input(bad))


def integration(out):
    cfg = load_config(ROOT / 'server/nex/config.local.json')
    cfg.pop('tls_port', None)
    reservations = []
    for key in ('auth_port', 'secure_port', 'nat_port', 'relay_port'):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM);s.bind(('127.0.0.1', 0));reservations.append(s);cfg[key] = s.getsockname()[1]
    path = out / 'config.json';path.write_text(json.dumps(cfg))
    bad = dict(cfg, bind='8.8.8.8');(out / 'bad.json').write_text(json.dumps(bad))
    try:
        load_config(out / 'bad.json')
    except ValueError:
        pass
    else:
        raise AssertionError('non-loopback configuration accepted')
    processes = []
    def start(index):
        log = out / f'server-{index}.jsonl'
        p = subprocess.Popen([sys.executable, '-m', 'server.nex', '--config', str(path), '--log', str(log), '--seconds', '8'],
                             cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        processes.append(p)
        deadline = time.monotonic()+5
        while time.monotonic()<deadline:
            if p.poll() is not None:
                raise AssertionError(p.communicate())
            if log.exists() and '"event": "ready"' in log.read_text():
                return p, log
            time.sleep(.02)
        raise AssertionError('local server startup timeout')
    try:
        for s in reservations:
            s.close()
        p, log = start(1)
        clients = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(4)]
        for s in clients:
            s.bind(('127.0.0.1', 0))
        for i, s in enumerate(clients):
            s.sendto(Packet(175, 161, 0x40, 0, 0, i).encode(), ('127.0.0.1', cfg['auth_port']))
        for _ in range(6000):
            clients[0].sendto(b'malformed', ('127.0.0.1', cfg['auth_port']))
        deadline = time.monotonic()+3
        while time.monotonic()<deadline:
            for i, s in enumerate(clients):
                s.sendto(Packet(175, 161, 0x40, 0, 0, 0x7777+i).encode(), ('127.0.0.1', cfg['secure_port']))
            time.sleep(.05)
            events = [json.loads(line) for line in log.read_text().splitlines()]
            endpoints = {tuple(e['endpoint']) for e in events if e['event']=='prudp-unverified' and 0x7777 <= e['sequence'] <= 0x777a}
            if len(endpoints) == 4:
                break
        assert len(endpoints) == 4 and p.poll() is None
        time.sleep(1.1)
        events = [json.loads(line) for line in log.read_text().splitlines()]
        assert any(e.get('malformed', 0) > 0 for e in events)
        p.terminate();p.wait(timeout=5)
        p2, _ = start(2)
        clients[1].sendto(Packet(175, 161, 0x40, 0, 0, 99).encode(), ('127.0.0.1', cfg['auth_port']))
        for s in clients:
            s.close()
        p2.terminate();p2.wait(timeout=5)
        return {'udp_probe': '6000 malformed datagrams; afterward four distinct local endpoints parsed; owned server restart/rebind succeeds',
                'scope': 'protocol-probe robustness only; no authenticated sessions, lobby, Pia traffic or game match'}
    finally:
        for p in processes:
            if p.poll() is None:
                p.kill()
            p.communicate(timeout=5)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path)
    a = ap.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(WireTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        return 1
    if a.out:
        out = a.out.resolve()
        if not out.is_relative_to(ROOT / 'build/run') or out.exists():
            ap.error('use a new build/run directory')
        out.mkdir(parents=True)
        evidence = integration(out)
        (out / 'results.json').write_text(json.dumps(evidence, indent=2)+'\n')
        print(json.dumps(evidence, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
