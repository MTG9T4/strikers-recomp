#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a Strikers relay answers: HELLO, three PINGs, LIST, BYE. Prints the ping and how many public rooms it lists.
    python relay_probe.py host[:port]"""
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strikers_relay as R  # noqa: E402


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    host, _, port = target.partition(':')
    port = int(port or 24890)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.connect((host, port))
    nonce = int.from_bytes(os.urandom(8), 'little')
    s.send(R.C_HEADER.pack(R.MAGIC, R.HELLO, R.PROTOCOL, 0, 0, 0) + struct.pack('<Q16sQ', 0, bytes(16), nonce))
    reply = s.recv(64)
    _, kind, _, _, token = R.S_HEADER.unpack_from(reply)
    if kind != R.WELCOME:
        print('FAIL: answered type', kind, 'field', token)
        return 1
    secret, echoed = struct.unpack_from('<QQ', reply, R.S_HEADER.size)
    assert echoed == nonce
    header = lambda k: R.C_HEADER.pack(R.MAGIC, k, R.PROTOCOL, 0, token, secret)
    pings = []
    for _ in range(3):
        t = time.perf_counter()
        s.send(header(R.PING) + struct.pack('<Q', 1))
        s.recv(64)
        pings.append((time.perf_counter() - t) * 1000)
    s.send(header(R.LIST))
    rooms = 0
    while True:
        d = s.recv(4096)
        if R.S_HEADER.unpack_from(d)[1] == R.LIST_END:
            break
        rooms += 1
    s.send(header(R.BYE))
    print(f'OK: {target} answers, ping {min(pings):.0f} ms, session {token:06x}; build 0 lists {rooms} rooms (a game build sees its own)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
