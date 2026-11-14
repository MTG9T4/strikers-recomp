"""Exercise production online service handlers against owned loopback peers.

No game execution, native DNS, certificate-store changes or external traffic.
    Includes actual OpenSSL TLS (positive, foreign CA, hostname, expiry negatives).
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid

from build_runtime import ROOT, msvc_environment
sys.path.insert(0, str(ROOT))
from server.account_stub import LocalAccountProvider
from server.nex.generate_pki import generate

HARNESS = r'''
#include <stdlib.h>
#include <string.h>
#include "network_policy.h"
static ImportDef defs[512];static int used;
void group_ldn_state(int state) {(void)state;}
void group_start(void) {}
void register_nnnet_imports(void);
void register_imports(const ImportDef *d,int n) {for(int i=0;i<n;i++){int k;for(k=0;k<used;k++)if(!strcmp(defs[k].name,d[i].name))break;defs[k]=d[i];if(k==used)used++;}}
void guest_trap(const char *why,uint64_t pc) {fprintf(stderr,"TRAP %s %llx\n",why,pc);abort();}
void os_init_event(uint64_t key,int autoclear) {(void)key;(void)autoclear;}
void os_signal_event(uint64_t key) {(void)key;}
void os_pin_host_thread(void) {}   /* audit 05: nnldn_lan.inc's worker calls it since audit 04 (os_threads.c is not linked here) */
__declspec(dllexport) void test_env(const char *name,const char *value) {_putenv_s(name,value);}
__declspec(dllexport) uint64_t test_call(const char *name,uint64_t *args) {
    if(!used){register_network_imports();register_nnnet_imports();}Ctx c={0};memcpy(c.x,args,8*8);
    for(int i=used-1;i>=0;i--)if(!strncmp(name,defs[i].name,strlen(name))){c.in_import=defs[i].name;defs[i].fn(&c);fflush(stdout);return c.x[0];}
    fprintf(stderr,"missing handler %s\n",name);abort();
}
'''


def address(ip, port):
    return bytes((16, 2)) + struct.pack('!H', port) + socket.inet_aton(ip) + bytes(8)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/run/job17-online-services')
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build/run') or out.exists():
        ap.error('use a new build/run directory')
    out.mkdir(parents=True)
    scratch = Path(tempfile.gettempdir()).resolve() / ('strikers-online-' + uuid.uuid4().hex)
    scratch.mkdir()
    lib = None
    result = {}
    try:
        src = scratch / 'test.c';src.write_text(HARNESS)
        env = msvc_environment();cl = Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        dll = scratch / 'online.dll'
        cmd = [str(cl), '/nologo', '/LD', '/MD', '/O2', '/std:c11', '/W3', '/we4013',
               '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
               *['/I' + str(ROOT / p) for p in ('runtime/core', 'runtime/services', 'build/gen')], str(src),
               *[str(ROOT / 'runtime/services' / n) for n in ('zt_transport.c', 'net_sim.c', 'network_policy.c', 'nnaccount.c', 'nnfriends.c',
                   'nnldn.c', 'nnnifm.c', 'nnsocket.c', 'nnssl.c', 'nnnet.c')], '/link', '/OUT:' + str(dll), 'ws2_32.lib']
        r = subprocess.run(cmd, env=env, cwd=scratch, capture_output=True, text=True)
        (out / 'build.log').write_text(r.stdout+r.stderr)
        assert r.returncode == 0, r.stdout+r.stderr
        valid = generate(out / 'pki', ['g01234567-lp1.s.n.srv.nintendo.net'])
        foreign = generate(out / 'foreign', ['g01234567-lp1.s.n.srv.nintendo.net'])
        expired = generate(out / 'expired', ['g01234567-lp1.s.n.srv.nintendo.net'], days=-1, signer=valid)
        provider = LocalAccountProvider(bytes(range(32)))
        token_file = out / 'player.token';provider.write_runtime_token(token_file, 1701)
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM);udp.bind(('127.0.0.1', 0));udp.settimeout(3)
        listener = socket.socket();listener.bind(('127.0.0.1', 0));listener.listen(4);listener.settimeout(8)
        ports = (udp.getsockname()[1], listener.getsockname()[1])
        lib = ctypes.CDLL(str(dll));lib.test_call.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)];lib.test_call.restype = ctypes.c_uint64
        lib.test_env.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        settings = {'STRIKERS_NETWORK': 'online', 'STRIKERS_ONLINE_SERVER': '127.0.0.1',
                    'STRIKERS_ONLINE_PORTS': ','.join(map(str, ports)), 'STRIKERS_ONLINE_CA_FILE': str(valid / 'ca.der'),
                    'STRIKERS_ONLINE_TOKEN_FILE': str(token_file)}
        settings['STRIKERS_ONLINE_OPENSSL'] = str(Path(sys.base_prefix) / 'DLLs/libssl-3.dll')
        for k, v in settings.items():
            lib.test_env(k.encode(), v.encode())
        def call(name, *args):
            packed = (ctypes.c_uint64 * 8)();buffers = []
            for i, arg in enumerate(args):
                if isinstance(arg, (bytes, bytearray)):
                    buf = ctypes.create_string_buffer(bytes(arg), max(1, len(arg)));buffers.append(buf);packed[i] = ctypes.addressof(buf)
                elif isinstance(arg, ctypes.Array) or isinstance(arg, ctypes._SimpleCData):
                    packed[i] = ctypes.addressof(arg)
                else:
                    packed[i] = arg
            value = ctypes.c_int64(lib.test_call(name.encode(), packed)).value
            return value
        so = lambda name, *args: call('nnsocket' + name, *args)
        acc = lambda name, *args: call('_ZN2nn7account' + str(len(name)) + name, *args)
        nifm = lambda name, *args: call('_ZN2nn4nifm' + str(len(name)) + name, *args)
        assert nifm('Initialize') == 0 and nifm('IsNetworkAvailable') == 1
        assert nifm('SubmitNetworkRequestAndWait') == 0
        ip = ctypes.c_uint32();assert nifm('GetCurrentPrimaryIpAddress', ip) == 0 and ip.value == 0x0100007f
        handle = struct.pack('<QQQ', 1, 0, 0);uid = ctypes.c_uint64()
        assert acc('GetNetworkServiceAccountId', uid, handle) == 0 and uid.value == 1701
        assert acc('GetNetworkServiceAccountId', uid, bytes(24)) != 0 and uid.value == 0
        async_ctx = ctypes.create_string_buffer(32);done = ctypes.c_uint8()
        assert acc('EnsureNetworkServiceAccountIdTokenCacheAsync', async_ctx, handle) == 0
        assert call('_ZN2nn7account12AsyncContext7HasDone', async_ctx, done) == 0 and done.value == 1
        assert call('_ZN2nn7account12AsyncContext9GetResult', async_ctx) == 0
        token_size = ctypes.c_uint64();token = ctypes.create_string_buffer(3078);token.raw = b'!' * len(token)
        assert acc('LoadNetworkServiceAccountIdTokenCache', token_size, token, 3073, handle) == 0
        assert provider.verify(token.value.decode()) == 1701 and token.raw[3073:] == b'!!!!!'
        guard = ctypes.create_string_buffer(b'guard', 6)
        assert acc('LoadNetworkServiceAccountIdTokenCache', token_size, guard, 2, handle) != 0 and guard.value == b'guard'
        result['account'] = 'local ID, async completion, opaque MAC token, invalid handle and short-buffer guards pass'
        result['nifm'] = 'connected only in configured online mode; primary address 127.0.0.1'
        fqdn=ctypes.create_string_buffer(256)
        source=b'g01234567-%.s.n.srv.nintendo.net'
        assert call('_ZN2nn3nsd9ResolveEx', fqdn, source+bytes(256-len(source))) == 0
        assert fqdn.value == b'g01234567-lp1.s.n.srv.nintendo.net'
        result['nsd'] = 'known SDK percent placeholder expands before DNS and TLS; no native resolver'
        assert so('Socket', 2, 3, 0) == -1
        fd = so('Socket', 2, 2, 17);assert fd >= 0
        assert so('Connect', fd, address('8.8.8.8', ports[0]), 16) == -1
        assert so('GetLastErrno') == 13
        assert call('_ZN2nn6socket6SendToEiPKvmi', fd, b'x', 1, 0, address('127.0.0.2', ports[0]), 16) == -1
        assert call('_ZN2nn6socket6SendToEiPKvmi', fd, b'x', 1, 0, address('127.0.0.1', ports[0]), 16) == 1
        payload, endpoint = udp.recvfrom(64);assert payload == b'x' and endpoint[0] == '127.0.0.1'
        udp.sendto(b'pong', endpoint)
        recv = ctypes.create_string_buffer(16);assert so('Recv', fd, recv, 16, 0) == 4 and recv.value == b'pong'
        assert so('Close', fd) == 0
        dns = ctypes.c_uint64()
        assert call('_ZN2nn6socket11GetAddrInfoEPKcS2_PK8', b'g01234567-lp1.s.n.srv.nintendo.net\0', b'10025\0', 0, dns) == 0
        ai = ctypes.string_at(dns.value, 48);assert struct.unpack_from('<I', ai, 4)[0] == 2
        assert ctypes.string_at(struct.unpack_from('<Q', ai, 32)[0], 16)[4:8] == b'\x7f\0\0\1'
        assert call('_ZN2nn6socket12FreeAddrInfoEP8', dns.value) == 0
        for name in ('example.com', 'g01234567-lp1.s.n.srv.nintendo.net.attacker', '127.0.0.1', 'api-lp1.znc.srv.nintendo.net/evil'):
            assert call('_ZN2nn6socket11GetAddrInfoEPKcS2_PK8', name.encode()+b'\0', 0, 0, dns) != 0 and dns.value == 0
        result['policy'] = 'public, unconfigured loopback, malformed/suffix-spoofed/numeric DNS blocked; UDP roundtrip uses only owned 127.0.0.1 endpoint'
        # Match main's import surface: the runtime provisions its replacement
        # SSL service, and the game starts with Context::Create, no Initialize.
        def tls_case(pki, hostname, expected, disable_verify=False):
            errors = []
            hold=threading.Event()
            def peer():
                try:
                    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(str(pki / 'server.pem'), str(pki / 'server.key'))
                    ctx.set_alpn_protocols(['http/1.1'])
                    raw, addr = listener.accept();assert addr[0] == '127.0.0.1'
                    with raw:
                        try:
                            with ctx.wrap_socket(raw, server_side=True) as tls:
                                if expected:
                                    assert tls.recv(4) == b'ping';tls.sendall(b'pong');hold.wait(3)
                                else:
                                    # Validation happens after the TLS exchange, before app IO.
                                    tls.settimeout(3)
                                    assert tls.recv(1) == b''
                        except (ssl.SSLError, ConnectionError):
                            if expected:
                                raise
                except BaseException as e:
                    errors.append(repr(e))
            worker = threading.Thread(target=peer);worker.start()
            tcp = so('Socket', 2, 1, 6);assert tcp >= 0
            n = so('Connect', tcp, address('127.0.0.1', ports[1]), 16);assert n in (0, -1)
            poll = ctypes.create_string_buffer(struct.pack('<iHH', tcp, 4, 0))
            assert so('Poll', poll, 1, 3000) == 1
            context = ctypes.c_uint64();connection = ctypes.create_string_buffer(64)
            assert call('_ZN2nn3ssl7Context6Create', context, 0x2000001) == 0
            ident = ctypes.c_uint64()
            assert call('_ZN2nn3ssl7Context15ImportServerPki', context, ident, b'pinned-game-pki', 15, 1) == 0
            assert call('nnsslConnectionCreate', connection, context) == 0
            # Hostname before socket is a valid SDK call order.
            name = hostname.encode()+b'\0'
            assert call('nnsslConnectionSetHostName', connection, name, len(name)) == 0
            assert call('nnsslConnectionSetSocketDescriptor', connection, tcp) == 0
            assert call('nnsslConnectionSetOption', connection, 0, 1) == 0  # caller retains the guest descriptor
            assert call('nnsslConnectionSetNextAlpnProto', connection, b'\x08http/1.1', 9) == 0
            if disable_verify:
                assert call('nnsslConnectionSetVerifyOption', connection, 0) == 0
            r = call('nnsslConnectionDoHandshakeWithCertBuffer', connection, 0, 0)
            assert (r == 0) == expected, (hostname, r, expected)
            count = ctypes.c_uint32()
            if expected:
                assert call('nnsslConnectionWrite', connection, b'ping', count, 4) == 0 and count.value == 4
                buf = ctypes.create_string_buffer(8)
                assert call('nnsslConnectionPeek', connection, buf, count, 4) == 0 and buf.value == b'pong'
                assert call('nnsslConnectionRead', connection, buf, count, 4) == 0 and buf.value == b'pong'
                assert call('nnsslConnectionGetVerifyCertError', connection, count) == 0 and count.value == 0
                state=ctypes.c_uint32();size=ctypes.c_uint32();alpn=ctypes.create_string_buffer(32)
                assert call('nnsslConnectionGetNextAlpnProto', connection, state, alpn, size, 32) == 0
                assert state.value == 1 and size.value == 8 and alpn.value == b'http/1.1'
                assert call('nnsslConnectionSetIoMode', connection, 2) == 0
                before=time.monotonic()
                assert call('nnsslConnectionPeek', connection, buf, count, 1) == 0x1987b
                assert time.monotonic()-before<.5, 'nonblocking TLS read stalled'
                hold.set()
            else:
                assert call('nnsslConnectionWrite', connection, b'ping', count, 4) != 0
                assert call('nnsslConnectionGetVerifyCertError', connection, count) != 0
            assert call('nnsslConnectionDestroy', connection) == 0
            assert call('nnsslContextDestroy', context) == 0 and context.value == 0
            assert so('Close', tcp) == 0
            worker.join(10);assert not worker.is_alive() and not errors, errors
        tls_case(valid, 'g01234567-lp1.s.n.srv.nintendo.net', True)
        tls_case(valid, 'nncs1-lp1.n.n.srv.nintendo.net', False, disable_verify=True)
        tls_case(foreign, 'g01234567-lp1.s.n.srv.nintendo.net', False, disable_verify=True)
        tls_case(expired, 'g01234567-lp1.s.n.srv.nintendo.net', False)
        result['tls'] = 'real OpenSSL ping/pong+peek; foreign CA, wrong hostname, expired certificate rejected; guest verify=0 cannot bypass'
        udp.close();listener.close()
        (out / 'results.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, indent=2))
    finally:
        if lib is not None:
            ctypes.CDLL('ucrtbase').fflush(None)
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(lib._handle))
        if scratch.resolve().parent != Path(tempfile.gettempdir()).resolve():
            raise RuntimeError('scratch cleanup escaped temp')
        shutil.rmtree(scratch)


if __name__ == '__main__':
    main()
