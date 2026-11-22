"""Internet rooms: production LDN/socket handlers in separate Windows processes find and join each other's rooms
through a local Strikers relay (server/relay/strikers_relay.py on 127.0.0.1), and only through it.

Each process gets its own same-network discovery port range, so the same-network path cannot find the other one.
Loopback only: no game, no public relay, no system setting. Evidence goes under --out in build/run.
"""
import argparse
import json
import os
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from build_runtime import ROOT, msvc_environment
import test_lan

HARNESS = r'''
#include "nnldn.c"
#include "nnsocket.c"
#include "network_policy.c"
#include "zt_transport.c"
#include "relay_transport.c"
#include "net_sim.c"
void group_ldn_state(int state) {(void)state;}
void group_start(void) {}
static ImportDef defs[512]; static int used;
void register_imports(const ImportDef *d,int n) {for(int i=0;i<n;i++)defs[used++]=d[i];}
void guest_trap(const char *why,uint64_t pc) {fprintf(stderr,"TRAP %s %llx\n",why,pc);abort();}
void os_init_event(uint64_t key,int autoclear) {(void)key;(void)autoclear;}
void os_signal_event(uint64_t key) {(void)key;}
void os_pin_host_thread(void) {}
void register_nnaccount_imports(void) {}
void register_nnfriends_imports(void) {}
void register_nnnifm_imports(void) {}
void register_nnssl_imports(void) {}
__declspec(dllexport) uint64_t test_call(const char *name,uint64_t *args) {
    if(!used)register_network_imports();Ctx c={0};memcpy(c.x,args,8*8);
    for(int i=0;i<used;i++)if(!strncmp(name,defs[i].name,strlen(name))){defs[i].fn(&c);fflush(stdout);return c.x[0];}
    fprintf(stderr,"missing handler %s\n",name);abort();
}
__declspec(dllexport) int test_errno(void) {return g_errno;}
'''


class Peer(test_lan.Peer):
    """test_lan.Peer with internet rooms on and its own discovery ports."""

    def __init__(self, dll, out, name, port, relay, key='', build=None, sim=None):
        settings = {'STRIKERS_NETWORK': 'internet', 'STRIKERS_LDN_PORT': str(port), 'STRIKERS_LDN_ID': uuid.uuid4().hex[:16],
                    'STRIKERS_LDN_INTERFACE': '127.0.0.1', 'STRIKERS_RELAY': relay, 'STRIKERS_ROOM_KEY': key,
                    'STRIKERS_PC_CONFIG': str(out / 'none.ini')}
        if build:
            settings['STRIKERS_LDN_BUILD'] = build
        if sim:
            settings['STRIKERS_NET_SIM'] = sim
        self.log = out / (name + '.log')
        self.process = subprocess.Popen([sys.executable, test_lan.__file__, '--worker', str(dll), str(self.log)],
            env={**{k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}, **settings},
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.responses = queue.Queue()
        threading.Thread(target=lambda: [self.responses.put(l) for l in self.process.stdout], daemon=True).start()
        try:
            assert self.ldn('Initialize')[0] == 0
        except BaseException:
            self.process.kill(); self.process.wait(timeout=4)
            raise

    def wait_log(self, text, seconds=6):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.ldn('GetState')  # each call flushes the worker's buffered log
            if self.log.exists() and text in self.log.read_text(errors='replace'):
                return True
            time.sleep(.1)
        return False


def free_udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/run/relay-contracts')
    args = parser.parse_args()
    out = args.out.resolve()
    assert out.is_relative_to(ROOT / 'build/run') and not out.exists(), 'use an unused build/run path'
    out.mkdir(parents=True)
    temp = Path(tempfile.gettempdir()) / ('strikers-relay-' + uuid.uuid4().hex)
    temp.mkdir()
    peers, result, server = [], {}, None
    try:
        src = temp / 'test.c'
        src.write_text(HARNESS)
        env = msvc_environment()
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        dll = temp / 'relay.dll'
        cmd = [str(cl), '/nologo', '/LD', '/MD', '/O2', '/std:c11', '/W3', '/we4013', '/D_CRT_SECURE_NO_WARNINGS',
               '/DWIN32_LEAN_AND_MEAN', *['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/services', 'build/gen')],
               str(src), '/link', '/OUT:' + str(dll), 'ws2_32.lib']
        build = subprocess.run(cmd, env=env, cwd=temp, capture_output=True, text=True)
        (out / 'build.log').write_text(build.stdout + build.stderr)
        assert build.returncode == 0, build.stdout + build.stderr
        relay_port = free_udp_port()
        server_log = open(out / 'relay.log', 'w')
        server = subprocess.Popen([sys.executable, '-I', str(ROOT / 'server/relay/strikers_relay.py'), '--addr', '127.0.0.1',
                                   '--port', str(relay_port), '-v'], stdout=server_log, stderr=subprocess.STDOUT)
        relay = f'127.0.0.1:{relay_port}'
        base = 32000 + int(uuid.uuid4().hex[:4], 16) % 9000
        ports = iter(range(base, base + 2000, 40))  # 40 apart: no two peers share a same-network discovery range

        host = Peer(dll, out, 'host', next(ports), relay); peers.append(host)
        sta = Peer(dll, out, 'station', next(ports), relay); peers.append(sta)
        assert host.wait_log('[relay] session') and sta.wait_log('[relay] session'), 'no relay session'
        result['sessions'] = 'passed, both connected to the local relay'
        assert sta.ldn('OpenStation')[0] == 0
        assert test_lan.scan(sta)[0] == 0
        assert host.ldn('OpenAccessPoint')[0] == 0
        cfg = bytearray(32); struct.pack_into('<QH', cfg, 0, 0x1019401051C000, 0)
        cfg[18] = 4; struct.pack_into('<H', cfg, 20, 7)
        sec = bytearray(68); struct.pack_into('<HH', sec, 0, 1, 4); sec[4:8] = b'test'
        assert host.ldn('SetAdvertiseData', b'internet-room', 13)[0] == 0
        assert host.ldn('CreateNetwork', cfg, sec, b'Host' + bytes(44))[0] == 0
        time.sleep(1.2)  # the host lists its room at its next tick
        count, info = test_lan.scan(sta); assert count == 1, count
        assert info[0x26c:0x279] == b'internet-room'
        assert sta.ldn('Connect', info, sec, b'Station' + bytes(41), 7, 0)[0] == 0
        joined = sta.ldn('GetNetworkInfo', bytes(0x480))[2][0]; assert joined[0x67] == 2
        hip = struct.unpack_from('<I', joined, 0x68)[0]; sip = struct.unpack_from('<I', joined, 0xa8)[0]
        result['find_and_join'] = 'passed: the room was found and joined through the relay only'
        hf = host.so('Socket', 2, 2, 17)[0]; sf = sta.so('Socket', 2, 2, 17)[0]
        for p, fd, ip in ((host, hf, hip), (sta, sf, sip)):
            assert p.so('Bind', fd, test_lan.address(ip, 4242), 16)[0] == 0
            assert p.so('SetSockOpt', fd, 0xffff, 0x20, struct.pack('<I', 1), 4)[0] == 0  # SO_BROADCAST
        payload = bytes(range(256)) * 4
        for frm, fd_from, to, fd_to, ip in ((sta, sf, host, hf, hip), (host, hf, sta, sf, sip)):
            assert frm.so('SendTo', fd_from, payload, len(payload), 0, test_lan.address(ip, 4242), 16)[0] == len(payload)
            assert to.so('Poll', struct.pack('<iHH', fd_to, 1, 0), 1, 2000)[0] == 1
            r, _, b = to.so('Recv', fd_to, bytes(2048), 2048, 0); assert r == len(payload) and b[1][:r] == payload
        assert host.so('SendTo', hf, b'broadcast', 9, 0, test_lan.address((hip & 0xffffff00) | 255, 4242), 16)[0] == 9
        assert sta.so('Poll', struct.pack('<iHH', sf, 1, 0), 1, 2000)[0] == 1
        assert sta.so('Recv', sf, bytes(16), 16, 0)[2][1][:9] == b'broadcast'
        result['game_datagrams'] = 'passed both ways and broadcast, 1024-byte payloads'

        keyed = Peer(dll, out, 'keyed', next(ports), relay, key='friends'); peers.append(keyed)
        other = Peer(dll, out, 'other-build', next(ports), relay, build='deadbeef'); peers.append(other)
        for p in (keyed, other):
            assert p.wait_log('[relay] session') and p.ldn('OpenStation')[0] == 0
            assert test_lan.scan(p)[0] == 0
        result['isolation'] = 'passed: a room key or another release does not see the public room'

        friend_host = Peer(dll, out, 'friend-host', next(ports), relay, key=' friends '); peers.append(friend_host)
        assert friend_host.wait_log('[relay] session') and friend_host.ldn('OpenAccessPoint')[0] == 0
        assert friend_host.ldn('CreateNetwork', cfg, sec, b'Friend' + bytes(42))[0] == 0
        time.sleep(1.2)
        count, info = test_lan.scan(keyed); assert count == 1
        assert test_lan.scan(other)[0] == 0
        far = Peer(dll, out, 'far-station', next(ports), relay, key='friends', sim='120,10,1'); peers.append(far)
        assert far.wait_log('[relay] session') and far.ldn('OpenStation')[0] == 0
        count, info = test_lan.scan(far); assert count == 1
        assert far.ldn('Connect', info, sec, b'Far' + bytes(45), 7, 0)[0] == 0
        result['room_key_and_distance'] = 'passed: same key (spaces trimmed) sees the room; 120 ms +-10, 1% loss station joins'
        assert keyed.ldn('Connect', info, sec, b'Keyed' + bytes(43), 7, 0)[0] == 0
        assert friend_host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x67] == 3
        result['three_players'] = 'passed: host, near and far station in one room'

        assert sta.ldn('Disconnect')[0] == 0
        time.sleep(.5)
        assert host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x67] == 1
        assert host.ldn('DestroyNetwork')[0] == 0
        time.sleep(.5)
        assert test_lan.scan(sta)[0] == 0
        result['leave_and_destroy'] = 'passed: leave reaches the host; a destroyed room leaves the list'
        friend_host.close(); peers.remove(friend_host)
        deadline = time.monotonic() + 3
        while keyed.ldn('GetState')[0] == 5 and time.monotonic() < deadline:
            time.sleep(.1)
        assert keyed.ldn('GetState')[0] == 4
        result['host_closes'] = 'passed: stations leave the room when its host finalizes'

        dead = Peer(dll, out, 'no-relay', next(ports), f'127.0.0.1:{free_udp_port()}'); peers.append(dead)
        assert dead.wait_log('does not answer', 9)
        assert dead.ldn('OpenStation')[0] == 0 and test_lan.scan(dead)[0] == 0
        result['relay_unreachable'] = 'passed: says so after 5 s, Local Wireless keeps working'
        result['status'] = 'PASS'
    finally:
        for peer in peers:
            try:
                peer.close()
            except Exception as e:  # noqa: BLE001
                result.setdefault('close_errors', []).append(repr(e))
        if server:
            server.terminate(); server.wait(timeout=5)
        shutil.rmtree(temp, ignore_errors=True)
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
