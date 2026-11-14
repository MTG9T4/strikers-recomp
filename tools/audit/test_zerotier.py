"""Production simulator contracts; --live opts into public ZeroTier roots only.

Live mode uses controller-less IPv6 ad-hoc networking and distinct persisted
identities. Datagram evidence is deliberately not called game-match evidence.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import shutil
import uuid
import re
from build_runtime import ROOT, msvc_environment

PROBE = r'''
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zt_transport.h"
#include "net_sim.h"
static int calls;
static int sent(uint32_t ip,uint16_t port,const void *b,unsigned n) {
 if(ip!=123||port!=456||n!=4||memcmp(b,"test",4))abort();calls++;return n;
}
int main(int argc,char **argv) {
 if(argc>1&&!strcmp(argv[1],"auth")) {
  zt_start();int waiting=0;uint64_t until=GetTickCount64()+5000;
  while(GetTickCount64()<until) {ZtStatus s;zt_status(&s);if(s.node>0xffffffffffULL)return 28;if(s.state==3)waiting=1;
   if(s.state==2){printf("AUTH waiting=%d node=%llx address=%s\n",waiting,s.node,s.address);return waiting&&s.node==0xabcdef0123ULL?0:23;}Sleep(20);}
  return 24;
 }
 if(argc>1&&(!strcmp(argv[1],"roots")||!strcmp(argv[1],"revoke"))) {
  /* audit 05: connected, then the roots stop answering (the state stays connected and usable) or the organizer
   * revokes this member (waiting for authorization, no traffic) */
  zt_start();uint64_t until=GetTickCount64()+5000;ZtStatus s;
  do {zt_status(&s);if(s.state==2)break;Sleep(20);} while(GetTickCount64()<until);
  if(s.state!=2)return 27;
  Sleep(2500);zt_status(&s);puts(s.message);
  if(!strcmp(argv[1],"roots"))return s.state==2&&zt_ready()?0:25;
  return s.state==3&&!zt_ready()?0:26;
 }
 if(argc>1&&!strcmp(argv[1],"config")) {
  char m[64],n[64];zt_config(m,sizeof m,n,sizeof n);
  printf("CONFIG mode=%s network=%s requested=%d\n",m,n,zt_requested());return 0;
 }
 if(argc>1&&!strcmp(argv[1],"invalid")) {
  zt_start();Sleep(100);ZtStatus s;zt_status(&s);
  printf("INVALID state=%d node=%llu\n",s.state,s.node);return s.state==4&&!s.node?0:20;
 }
 if(argc==1) {
  if(!net_sim_init("100,0,0"))return 1;
  if(net_sim_send(1000,123,456,"test",4,sent)!=4||calls)return 2;
  net_sim_flush(1099,sent);if(calls)return 3;net_sim_flush(1100,sent);if(calls!=1)return 4;
  net_sim_init("250,50,0");calls=0;
  for(int i=0;i<100;i++)net_sim_send(0,123,456,"test",4,sent);
  net_sim_flush(199,sent);if(calls)return 5;net_sim_flush(300,sent);if(calls!=100)return 6;
  net_sim_init("0,0,100");calls=0;for(int i=0;i<100;i++)net_sim_send(0,123,456,"test",4,sent);
  net_sim_flush(10000,sent);if(calls)return 7;
  net_sim_init("200,0,2");calls=0;for(int i=0;i<4000;i++)net_sim_send(0,123,456,"test",4,sent);
  net_sim_flush(200,sent);if(calls<3850||calls>3970)return 8;
  if(net_sim_init("1,2,3,4")||net_sim_init("-1,0,0")||net_sim_init("0,0,101"))return 9;
  net_sim_init(NULL);calls=0;net_sim_send(0,123,456,"test",4,sent);if(calls!=1)return 10;
  net_sim_init("100,0,0");calls=0;net_sim_send(0,123,456,"test",4,sent);
  net_sim_init(NULL);net_sim_flush(1000,sent);if(calls)return 21;
  net_sim_init("100,0,0");calls=0;for(int i=0;i<5000;i++)net_sim_send(0,123,456,"test",4,sent);
  net_sim_flush(1000,sent);if(calls!=4096)return 22;
  puts("PASS simulator: deadline, jitter, 100%/2% loss, invalid config, disabled fast path");return 0;
 }
 zt_start();uint64_t until=GetTickCount64()+atoi(argv[1])*1000ULL,last=0;int opened=0,rx=0;
 while(GetTickCount64()<until) {
  ZtStatus s;zt_status(&s);
  if(zt_ready()&&!opened){uint16_t port;if(!(opened=zt_open(24872,&port)))return 11;}
  if(GetTickCount64()-last>1000) {
   last=GetTickCount64();printf("STATUS state=%d node=%010llx network=%016llx address=%s peers=%u rx=%d\n",s.state,s.node,s.network,s.address,s.peers,rx);
   for(unsigned i=0;i<s.peers;i++)printf("PEER id=%010llx path=%d latency=%d\n",s.peer[i].id,s.peer[i].path,s.peer[i].latency);
   if(opened)zt_discover(24872,"ZTPROBE",7);fflush(stdout);
  }
  if(opened)for(int i=0;i<128;i++) {char b[128];uint32_t token;uint16_t port;int n=zt_recv(&token,&port,b,sizeof b);if(n<0)break;
   if(n==7&&!memcmp(b,"ZTPROBE",7)){rx++;printf("RECV token=%08x port=%u bytes=%d\n",token,port,n);fflush(stdout);}
  }
  Sleep(2);
 }
 zt_close();printf("RESULT rx=%d\n",rx);return rx?0:12;
}
'''

MOCK = r'''
#define ADD_EXPORTS 1
#include <winsock2.h>
#include <windows.h>
#include <string.h>
#include "ZeroTierSockets.h"
static DWORD at;
int zts_init_from_storage(const char *p){return 0;}
int zts_init_set_event_handler(void (*cb)(void*)){return 0;}
int zts_init_allow_port_mapping(unsigned p){return 0;}
int zts_init_allow_net_cache(unsigned p){return 0;}
int zts_node_start(void){at=GetTickCount();return 0;}
int zts_node_stop(void){return 0;}
static int phase(void){const char *e=getenv("ZT_MOCK_PHASE");return e?atoi(e):0;}
int zts_node_is_online(void){return !(phase()==1&&GetTickCount()-at>1500);}
uint64_t zts_node_get_id(void){return GetTickCount()-at<100?UINT64_MAX:0xabcdef0123ULL;}
int zts_net_join(uint64_t id){return 0;}
int zts_net_transport_is_ready(uint64_t id){return GetTickCount()-at>750;}
int zts_net_get_status(uint64_t id){DWORD t=GetTickCount()-at;if(phase()==2&&t>1500)return ZTS_NETWORK_STATUS_ACCESS_DENIED;return t>750?ZTS_NETWORK_STATUS_OK:ZTS_NETWORK_STATUS_ACCESS_DENIED;}
int zts_net_get_name(uint64_t id,char *p,unsigned n){strcpy_s(p,n,"Mock private group");return 0;}
int zts_addr_get_str(uint64_t id,unsigned f,char *p,unsigned n){strcpy_s(p,n,"10.55.0.1");return 0;}
int zts_addr_compute_6plane_str(uint64_t id,uint64_t node,char *p,unsigned n){return -1;}
int zts_socket(int f,int t,int p){return -1;}
int zts_bind(int fd,const char *p,unsigned short port){return -1;}
int zts_bsd_close(int fd){return 0;}
int zts_bsd_setsockopt(int fd,int l,int o,const void *v,zts_socklen_t n){return -1;}
int zts_bsd_fcntl(int fd,int cmd,int flags){return -1;}
ssize_t zts_bsd_sendto(int fd,const void *p,size_t n,int f,const struct zts_sockaddr *a,zts_socklen_t l){return -1;}
ssize_t zts_bsd_recvfrom(int fd,void *p,size_t n,int f,struct zts_sockaddr *a,zts_socklen_t *l){return -1;}
int zts_util_ipstr_to_saddr(const char *p,unsigned short port,struct zts_sockaddr *a,zts_socklen_t *l){return -1;}
'''


def auth_and_config_contracts(exe):
    """No network: SDK test double exercises the actual background worker/UI state."""
    tmp = Path(tempfile.gettempdir()) / ('strikers-zt-auth-' + uuid.uuid4().hex)
    tmp.mkdir()
    try:
        env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
        config = tmp / 'strikers.ini'
        config.write_text('[Network]\nmode=zerotier\nzerotier_network=aabbccddeeff0011\n')
        env.update(STRIKERS_PC_CONFIG=config.as_posix(), STRIKERS_USERDATA=(tmp / 'userdata').as_posix())
        r = subprocess.run([str(exe), 'config'], env=env, capture_output=True, text=True, check=True)
        assert 'network=aabbccddeeff0011 requested=1' in r.stdout
        r = subprocess.run([str(exe), 'config'], env=dict(env, STRIKERS_NETWORK='offline'), capture_output=True, text=True, check=True)
        assert 'mode=offline' in r.stdout and 'requested=0' in r.stdout
        source = tmp / 'sdk_mock.c'; source.write_text(MOCK)
        tool_env = msvc_environment()
        cl = Path(tool_env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'
        r = subprocess.run([str(cl), '/nologo', '/LD', '/MD', '/O2', '/std:c11',
            '/I' + str(ROOT / 'deps/libzt/include'), str(source), '/Fe:' + str(tmp / 'strikers_zt.dll'),
            '/link', '/EXPORT:zts_util_ipstr_to_saddr'],
            cwd=tmp, env=tool_env, capture_output=True, text=True)
        assert r.returncode == 0, r.stdout + r.stderr
        shutil.copy2(exe, tmp / 'probe.exe')
        subprocess.run([str(tmp / 'probe.exe'), 'auth'], env=env, timeout=10, check=True)
        print('PASS config precedence and authorization-denied -> connected (mock SDK, no traffic)')
        subprocess.run([str(tmp / 'probe.exe'), 'roots'], env=dict(env, ZT_MOCK_PHASE='1'), timeout=15, check=True)
        subprocess.run([str(tmp / 'probe.exe'), 'revoke'], env=dict(env, ZT_MOCK_PHASE='2'), timeout=15, check=True)
        print('PASS roots unreachable after connecting keeps the transport; revoked authorization stops it (mock SDK, audit 05)')
    finally:
        assert tmp.resolve().parent == Path(tempfile.gettempdir()).resolve()
        shutil.rmtree(tmp)


def build_probe(live=False):
    env = msvc_environment()
    exe = ROOT / ('build/bin/strikers_job18_zt_live.exe' if live else 'build/bin/strikers_job18_zt_probe.exe')
    tmp = Path(tempfile.gettempdir()) / ('strikers-zt-' + uuid.uuid4().hex)
    tmp.mkdir()
    try:
        src = Path(tmp) / 'probe.c'
        src.write_text(PROBE)
        cmd = [str(Path(env['VCToolsInstallDir']) / 'bin/Hostx64/x64/cl.exe'), '/nologo', '/MD', '/O2',
               '/std:c11', '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
               '/I' + str(ROOT / 'runtime/services'), str(src),
               str(ROOT / 'runtime/services/zt_transport.c'), str(ROOT / 'runtime/services/net_sim.c'),
               '/Fe:' + str(exe), '/link', 'ws2_32.lib']
        r = subprocess.run(cmd, cwd=tmp, env=env, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(r.stdout + r.stderr)
    finally:
        assert tmp.resolve().parent == Path(tempfile.gettempdir()).resolve()
        shutil.rmtree(tmp)
    return exe


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--live', type=int, choices=(2, 3))
    p.add_argument('--group-live', type=int, choices=(2, 3), help='real ad-hoc nodes with native group clients and local service; never launches the game')
    p.add_argument('--service-url', help='existing loopback Worker for --group-live; otherwise owns the Node/SQLite fallback')
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--block-direct', action='store_true', help='reject direct paths to the other test nodes inside libzt')
    p.add_argument('--profiles', type=Path, help='reuse an earlier live run\'s persisted player identities')
    p.add_argument('--out', type=Path, default=ROOT / 'build/run/job18-zt-live')
    a = p.parse_args()
    if a.group_live:
        from zt_group_live import run
        run(a.group_live, a.out, a.service_url)
        return
    exe = build_probe(bool(a.live))
    subprocess.run([str(exe)], check=True)
    auth_and_config_contracts(exe)
    env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_NETWORK='zerotier', STRIKERS_ZEROTIER_NETWORK='invalid')
    subprocess.run([str(exe), 'invalid'], env=env, check=True)
    if not a.live:
        return
    out = a.out.resolve()
    assert out.is_relative_to(ROOT / 'build/run') and not out.exists()
    out.mkdir(parents=True)
    lib = ctypes.CDLL(str(ROOT / 'build/bin/strikers_zt.dll'))
    profiles = a.profiles.resolve() if a.profiles else out
    assert profiles.is_relative_to(ROOT / 'build/run')
    ids = []
    for i in range(a.live):
        folder = profiles / f'player{i}/zerotier'
        if a.profiles:
            identity = (folder / 'identity.secret').read_bytes()
        else:
            folder.mkdir(parents=True)
            key = ctypes.create_string_buffer(384)
            length = ctypes.c_uint(384)
            assert lib.zts_id_new(key, ctypes.byref(length)) == 0
            identity = key.value
            (folder / 'identity.secret').write_bytes(identity)
        ids.append(identity.decode().split(':')[0])
    processes, files, result = [], [], []
    try:
        for i in range(a.live):
            env = {k: v for k, v in os.environ.items() if not k.startswith('STRIKERS_')}
            env.update(STRIKERS_NETWORK='zerotier', STRIKERS_ZEROTIER_NETWORK='ff61286137000000',
                       STRIKERS_ZT_PEERS=','.join(n for n in ids if n != ids[i]),
                       STRIKERS_USERDATA=(profiles / f'player{i}').as_posix())
            if a.block_direct:
                env['STRIKERS_ZT_BLOCK_DIRECT'] = env['STRIKERS_ZT_PEERS']
            f = (out / f'player{i}.log').open('wb'); files.append(f)
            child = subprocess.Popen([str(exe), str(a.seconds)], env=env, cwd=ROOT / 'build/bin', stdout=f, stderr=subprocess.STDOUT)
            processes.append(child)
        for child in processes:
            child.wait(timeout=a.seconds + 15)
    finally:
        for i, child in enumerate(processes):
            if child.poll() is None:
                child.kill(); child.wait()
            result.append({'node': ids[i], 'exit': child.returncode, 'log': f'player{i}.log'})
        for f in files:
            f.close()
        for i, record in enumerate(result):
            log = (out / record['log']).read_text(errors='replace')
            matches = re.findall(r'RESULT rx=(\d+)', log)
            record['received'] = int(matches[-1]) if matches else 0
            record['paths'] = {node: re.findall(rf'PEER id={node} path=(-?\d+) latency=(-?\d+)', log)
                               for node in ids if node != ids[i]}
            record['direct_rejections_logged'] = '[zt-path-sim] rejected direct path' in log
            nodes = re.findall(r'STATUS state=2 node=([0-9a-f]+)', log)
            record['persisted_node_verified'] = bool(nodes) and all(n == ids[i] for n in nodes)
        (out / 'results.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))
    assert all(r['exit'] == 0 for r in result), 'real ZeroTier transport not verified; inspect retained logs'
    for r in result:
        assert r['received'] > 0
        assert r['persisted_node_verified'], 'persisted identity changed'
        for values in r['paths'].values():
            assert values, 'peer metrics missing'
            if a.block_direct:
                assert not any(path == '1' for path, latency in values), 'direct path bypassed test policy'
                assert r['direct_rejections_logged'], 'path rejection was never exercised'
            else:
                assert any(path == '1' for path, latency in values), 'direct path not established'


if __name__ == '__main__':
    main()
