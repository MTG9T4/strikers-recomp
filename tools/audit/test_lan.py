"""Production LDN/socket handlers in independent Windows processes; loopback only.

Build scratch is temporary. Evidence is retained under --out in build/run.
No game binary, system setting, firewall change, DNS, or public endpoint.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import time
import uuid
import sys
import queue
import threading
from build_runtime import ROOT, msvc_environment


def worker(dll, log):
    protocol = os.fdopen(os.dup(1), 'w', buffering=1)
    with open(log, 'w') as f:
        os.dup2(f.fileno(), 1)
        os.dup2(f.fileno(), 2)
        lib = ctypes.CDLL(dll)
        lib.test_call.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)]
        lib.test_call.restype = ctypes.c_uint64
        lib.test_errno.restype = ctypes.c_int
        try:
            while True:
                command = json.loads(sys.stdin.readline())
                if command is None:
                    break
                name, args = command
                buffers = {}
                packed = (ctypes.c_uint64 * 8)()
                for i, arg in enumerate(args):
                    if isinstance(arg, dict):
                        data = bytes.fromhex(arg['hex'])
                        buffers[i] = ctypes.create_string_buffer(data, max(1, len(data)))
                        packed[i] = ctypes.addressof(buffers[i])
                    else:
                        packed[i] = arg
                result = lib.test_call(name.encode(), packed)
                protocol.write(json.dumps((ctypes.c_int64(result).value, lib.test_errno(), {i: b.raw.hex() for i, b in buffers.items()})) + '\n')
        finally:
            ctypes.CDLL('ucrtbase').fflush(None)


class Peer:
    def __init__(self, dll, out, name, port, build=None, interface='127.0.0.1'):
        settings = {'STRIKERS_NETWORK': 'lan',
                    'STRIKERS_LDN_PORT': str(port), 'STRIKERS_LDN_ID': uuid.uuid4().hex[:16]}
        if interface is not None:
            settings['STRIKERS_LDN_INTERFACE'] = interface
        if build:
            settings['STRIKERS_LDN_BUILD'] = build
        self.process = subprocess.Popen([sys.executable, __file__, '--worker', str(dll), str(out / (name + '.log'))],
            env={**{k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}, **settings}, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        self.responses = queue.Queue()
        def read():
            for line in self.process.stdout:
                self.responses.put(line)
        threading.Thread(target=read, daemon=True).start()
        try:
            assert self.ldn('Initialize')[0] == 0
        except BaseException:
            self.process.kill(); self.process.wait(timeout=4)
            raise

    def call(self, name, *args):
        self.process.stdin.write(json.dumps((name, [{'hex': bytes(a).hex()} if isinstance(a, (bytes, bytearray)) else a for a in args])) + '\n')
        self.process.stdin.flush()
        try:
            r, e, b = json.loads(self.responses.get(timeout=8))
        except queue.Empty:
            raise AssertionError('handler hung: ' + name)
        return r, e, {int(i): bytes.fromhex(v) for i, v in b.items()}

    def ldn(self, name, *args):
        return self.call(f'_ZN2nn3ldn{len(name)}{name}', *args)

    def so(self, name, *args):
        return self.call(f'_ZN2nn6socket{len(name)}{name}', *args)

    def close(self):
        if self.process.poll() is None:
            self.ldn('Finalize')
            self.process.stdin.write('null\n'); self.process.stdin.flush()
            self.process.wait(timeout=4)


def address(ip, port):
    return bytes([16, 2]) + struct.pack('!HI', port, ip) + bytes(8)


def scan(p):
    r, _, b = p.ldn('Scan', bytes(0x480 * 8), bytes(4), 8, bytes(0x60), 0)
    assert r == 0
    return struct.unpack('<I', b[1])[0], b[0][:0x480]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'build/run/job15-lan-contracts')
    parser.add_argument('--adapter-smoke', action='store_true', help='also bind auto-selected LAN adapter; sends no discovery/data packets')
    parser.add_argument('--adapter-ip', help='explicit local adapter for --adapter-smoke; sends no packets')
    args = parser.parse_args()
    out = args.out.resolve()
    assert out.is_relative_to(ROOT / 'build/run') and not out.exists(), 'use an unused build/run path'
    out.mkdir(parents=True)
    temp = Path(tempfile.gettempdir()) / ('strikers-lan-' + uuid.uuid4().hex)
    temp.mkdir()
    peers = []
    result = {}
    try:
        src = temp / 'test.c'
        src.write_text(r'''
#include "nnldn.c"
#include "nnsocket.c"
#include "network_policy.c"
#include "zt_transport.c"
#include "relay_transport.c"
#include "net_sim.c"
/* Group HTTP/overlay is tested separately; this harness owns only LDN sockets. */
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
''')
        env = msvc_environment()
        cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        dll = temp / 'lan.dll'
        cmd = [str(cl), '/nologo', '/LD', '/MD', '/O2', '/std:c11', '/W3', '/we4013',
               '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
               *['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/services', 'build/gen')],
               str(src), '/link', '/OUT:' + str(dll), 'ws2_32.lib']
        build = subprocess.run(cmd, env=env, cwd=temp, capture_output=True, text=True)
        (out / 'build.log').write_text(build.stdout + build.stderr)
        assert build.returncode == 0, build.stdout + build.stderr
        # Reserve a fresh range; production bind is exclusive and reports exhaustion.
        port = 32000 + int(uuid.uuid4().hex[:4], 16) % 12000
        host = Peer(dll, out, 'host', port); peers.append(host)
        sta = Peer(dll, out, 'station', port); peers.append(sta)
        assert sta.ldn('OpenStation')[0] == 0
        assert scan(sta)[0] == 0
        result['empty_scan'] = 'passed, bounded 600ms'
        assert host.ldn('OpenAccessPoint')[0] == 0
        assert host.ldn('SetStationAcceptPolicy', 1)[0] == 0
        assert host.ldn('SetAdvertiseData', b'pre-create-advertise', 20)[0] == 0
        cfg = bytearray(32); struct.pack_into('<QH', cfg, 0, 0x1019401051C000, 0)
        cfg[18] = 8; struct.pack_into('<H', cfg, 20, 7)
        sec = bytearray(68); struct.pack_into('<HH', sec, 0, 1, 4); sec[4:8] = b'test'
        assert host.ldn('CreateNetwork', cfg, sec, b'Host' + bytes(44))[0] == 0
        assert host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x62] == 1
        assert host.ldn('SetStationAcceptPolicy', 0)[0] == 0
        assert host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x26c:0x280] == b'pre-create-advertise'
        key_api = '_ZN2nn7friends29GetPlayHistoryRegistrationKey'
        r, _, key = host.call(key_api, bytes(64) + b'guard', struct.pack('<QQ', 1, 0), 1)
        assert r == 0 and key[0][:8] == b'SLDNHIST' and key[0][64:] == b'guard'
        assert host.call(key_api, bytes(64), struct.pack('<QQ', 1, 0), 0)[0] != 0
        result['local_play_history_key'] = 'passed 64-byte bounds; Internet/account key requests still refused'
        assert host.ldn('SetAdvertiseData', b'advertise', 9)[0] == 0
        count, info = scan(sta); assert count == 1
        assert info[0x26c:0x275] == b'advertise' and info[0x66:0x68] == bytes([8, 1])
        assert sta.ldn('Connect', info, sec, b'Station' + bytes(41), 7, 0)[0] == 0
        r, _, b = sta.ldn('GetNetworkInfo', bytes(0x480)); assert r == 0
        joined = b[0]; assert joined[0x67] == 2
        hip = struct.unpack_from('<I', joined, 0x68)[0]
        sip = struct.unpack_from('<I', joined, 0xa8)[0]
        assert hip != sip and hip >> 16 == sip >> 16 == 0xa9fe
        result['discovery_join_layouts'] = {'host': hex(hip), 'station': hex(sip), 'nodes': 2}
        hf = host.so('Socket', 2, 2, 17)[0]; sf = sta.so('Socket', 2, 2, 17)[0]
        for p, fd, ip in ((host, hf, hip), (sta, sf, sip)):
            assert p.so('Bind', fd, address(ip, 4242), 16)[0] == 0
            assert p.so('Fcntl', fd, 4, 0x800)[0] == 0
            assert p.so('Fcntl', fd, 3)[0] == 0x800
            assert p.so('SetSockOpt', fd, 0, 4, struct.pack('<I', 64), 4)[0] == 0
            assert p.so('GetSockOpt', fd, 0, 4, bytes(4), struct.pack('<I', 4))[2][3] == struct.pack('<I', 64)
            assert p.so('Recv', fd, bytes(16), 16, 0)[:2] == (-1, 11)
            assert p.so('SetSockOpt', fd, 0xffff, 0x20, struct.pack('<I', 1), 4)[0] == 0
            r, _, b = p.so('GetSockName', fd, bytes(16), struct.pack('<I', 16))
            assert r == 0 and b[1] == address(ip, 4242)
        payload = bytes(range(256)) * 4
        assert sta.so('SendTo', sf, payload, len(payload), 0, address(hip, 4242), 16)[0] == len(payload)
        poll = struct.pack('<iHH', hf, 1, 0)
        r, _, b = host.so('Poll', poll, 1, 1000); assert r == 1 and struct.unpack_from('<H', b[0], 6)[0] == 1
        r, _, b = host.so('RecvFrom', hf, bytes(2048), 2048, 2, bytes(16), struct.pack('<I', 16))
        assert r == len(payload) and b[1][:r] == payload and b[4] == address(sip, 4242)
        assert host.so('Recv', hf, bytes(5), 5, 0)[0] == 5  # truncation consumes one datagram
        assert host.so('Recv', hf, bytes(5), 5, 0)[:2] == (-1, 11)
        for ip in (0x08080808, 0x7f000001, 0xc0a80101, hip + 100):
            assert sta.so('SendTo', sf, b'blocked', 7, 0, address(ip, 4242), 16)[:2] == (-1, 13)
        result['udp_socket_contracts'] = 'passed unicast, source translation, peek, truncation, nonblocking, poll, policy'
        assert sta.so('Fcntl', sf, 4, 0)[0] == 0
        assert sta.so('SetSockOpt', sf, 0xffff, 0x1006, struct.pack('<qq', 0, 20000), 16)[0] == 0
        start = time.monotonic()
        assert sta.so('Recv', sf, bytes(16), 16, 0)[:2] == (-1, 11)
        assert .010 <= time.monotonic() - start < 1
        assert sta.so('Fcntl', sf, 4, 0x800)[0] == 0
        assert sta.so('Poll', struct.pack('<iHH', -1, 1, 99), 1, 0)[2][0] == struct.pack('<iHH', -1, 1, 0)
        assert sta.so('Poll', struct.pack('<iHH', 127, 1, 0), 1, 0)[2][0] == struct.pack('<iHH', 127, 1, 0x20)
        assert sta.so('GetAddrInfo', b'nintendo.invalid\0', 0, 0, bytes(8))[0] == 2
        result['timeouts_poll_dns'] = 'passed blocking receive timeout, ignored/invalid descriptors, resolver refusal'
        assert host.so('SendTo', hf, b'broadcast', 9, 0, address((hip & 0xffffff00) | 255, 4242), 16)[0] == 9
        assert sta.so('Poll', struct.pack('<iHH', sf, 1, 0), 1, 1000)[0] == 1
        assert sta.so('Recv', sf, bytes(16), 16, 0)[2][1][:9] == b'broadcast'
        assert host.so('Recv', hf, bytes(16), 16, 0)[2][1][:9] == b'broadcast'
        # Flood the production receive socket with malformed datagrams, then verify
        # a valid heartbeat and data delivery still progress and the API is bounded.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as flood:
            for _ in range(3000):
                for slot in range(2):
                    flood.sendto(b'SLDN' + bytes(70), ('127.0.0.1', port + slot))
        assert sta.so('SendTo', sf, b'after-flood', 11, 0, address(hip, 4242), 16)[0] == 11
        assert host.so('Poll', poll, 1, 1500)[0] == 1
        assert host.so('Recv', hf, bytes(16), 16, 0)[2][1][:11] == b'after-flood'
        result['malformed_flood'] = 'passed 6000 packets, subsequent datagram delivered'
        assert host.ldn('Reject', sip)[0] == 0
        time.sleep(.1); assert sta.ldn('GetState')[0] == 4 and sta.ldn('GetDisconnectReason')[0] == 5
        count, info = scan(sta); assert count == 1
        assert host.ldn('SetStationAcceptPolicy', 1)[0] == 0
        assert sta.ldn('Connect', info, sec, bytes(48), 7, 0)[0] != 0
        assert host.ldn('SetStationAcceptPolicy', 0)[0] == 0
        assert sta.ldn('Connect', info, sec, bytes(48), 8, 0)[0] == 0xe4cb
        assert sta.ldn('Connect', info, sec, bytes(48), 6, 0)[0] == 0xe2cb
        wrong = bytearray(sec); wrong[4] ^= 1
        assert sta.ldn('Connect', info, wrong, bytes(48), 7, 0)[0] == 0x28cb
        assert sta.ldn('Connect', info, sec, bytes(48), 7, 0)[0] == 0
        assert sta.ldn('Disconnect')[0] == 0
        time.sleep(.1)
        assert host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x67] == 1
        result['reject_leave_security_accept_policy'] = 'passed'
        mismatch = Peer(dll, out, 'mismatch', port, 'deadbeef'); peers.append(mismatch)
        assert mismatch.ldn('OpenStation')[0] == 0
        count, info = scan(mismatch); assert count == 1
        assert mismatch.ldn('Connect', info, sec, bytes(48), 7, 0)[0] != 0
        result['build_mismatch'] = 'passed, visible in scan and refused at connect'
        count, info = scan(sta); assert count == 1
        assert sta.ldn('Connect', info, sec, bytes(48), 7, 0)[0] == 0
        third = Peer(dll, out, 'station2', port); peers.append(third)
        assert third.ldn('OpenStation')[0] == 0
        count, info = scan(third); assert count == 1
        assert third.ldn('Connect', info, sec, b'Third' + bytes(43), 7, 0)[0] == 0
        r, _, b = third.ldn('GetNetworkInfo', bytes(0x480)); assert r == 0
        tip = struct.unpack_from('<I', b[0], 0xe8)[0]
        tf = third.so('Socket', 2, 2, 17)[0]
        assert third.so('Bind', tf, address(tip, 4242), 16)[0] == 0
        assert third.so('Fcntl', tf, 4, 0x800)[0] == 0
        assert sta.so('SendTo', sf, b'relay', 5, 0, address(tip, 4242), 16)[0] == 5
        assert third.so('Poll', struct.pack('<iHH', tf, 1, 0), 1, 1000)[0] == 1
        assert third.so('RecvFrom', tf, bytes(16), 16, 0, bytes(16), struct.pack('<I', 16))[2][4] == address(sip, 4242)
        third.process.kill(); third.process.wait(timeout=4)
        time.sleep(6.7)
        assert host.ldn('GetNetworkInfo', bytes(0x480))[2][0][0x67] == 2
        result['station_relay_and_process_loss'] = 'passed, third node forwarded by AP then expired after 6s'
        assert host.ldn('DestroyNetwork')[0] == 0
        time.sleep(.1)
        assert sta.ldn('GetDisconnectReason')[0] == 3
        sp = bytes(range(32))
        entry = struct.pack('<I', sip) + joined[0xa8+4:0xa8+10] + bytes(2)
        assert host.ldn('CreateNetworkPrivate', cfg, sec, sp, bytes(48), 1, entry)[0] == 0
        assert host.ldn('SetStationAcceptPolicy', 3)[0] == 0
        assert scan(sta)[0] == 0
        r, _, b = sta.ldn('ScanPrivate', bytes(0x480 * 8), bytes(4), 8, bytes(0x60), 0)
        assert r == 0 and struct.unpack('<I', b[1])[0] == 1
        assert sta.ldn('ConnectPrivate', cfg, sec, sp, bytes(48), 7, 0)[0] == 0
        assert host.ldn('GetSecurityParameter', bytes(32))[2][0] == sp
        assert host.ldn('DestroyNetwork')[0] == 0
        time.sleep(.1)
        assert sta.ldn('GetState')[0] == 4
        assert host.ldn('SetStationAcceptPolicy', 0)[0] == 0
        assert host.ldn('CreateNetwork', cfg, sec, bytes(48))[0] == 0
        count, info = scan(sta); assert count == 1
        assert sta.ldn('Connect', info, sec, bytes(48), 7, 0)[0] == 0
        result['private_room_and_explicit_destroy'] = 'passed ScanPrivate, ConnectPrivate, parameters, whitelist, destroy reason'
        host.process.kill(); host.process.wait(timeout=4)
        deadline = time.monotonic() + 7
        while sta.ldn('GetState')[0] == 5 and time.monotonic() < deadline:
            time.sleep(.1)
        assert sta.ldn('GetState')[0] == 4 and sta.ldn('GetDisconnectReason')[0] == 6
        result['host_process_loss'] = 'passed, signal-lost after 6s'
        if args.adapter_smoke:
            adapter = Peer(dll, out, 'adapter-bind', port, interface=args.adapter_ip); peers.append(adapter)
            assert adapter.ldn('GetState')[0] == 1
            adapter.close()
            result['adapter_bind_only'] = 'passed selected adapter and exclusive Winsock bind; no AP/scan/packets; see adapter-bind.log'
        result['status'] = 'PASS'
    finally:
        for peer in peers:
            peer.close()
        assert temp.resolve().parent == Path(tempfile.gettempdir()).resolve()
        shutil.rmtree(temp)
        (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--worker':
        worker(sys.argv[2], sys.argv[3])
    else:
        main()
