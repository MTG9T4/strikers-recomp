"""Two isolated games, mock SDK, loopback SQLite Worker handler; no public traffic.

Requires the default optimized build. The local test host reuses its objects,
recompiling only group_client.c with the explicit numeric-loopback HTTP exception.
Run --name UNIQUE; operate the owned test PIDs using pc_window_probe.py while it
runs. The SDK double can create a room but does not carry gameplay datagrams.
This runner is UI evidence, not a ZeroTier match or wrangler/D1 acceptance test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import ctypes
from build_runtime import ROOT, msvc_environment
from test_zerotier import MOCK
from test_group_service import call

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name',required=True)
    ap.add_argument('--seconds',type=int,default=300)
    ap.add_argument('--enrolled-from',type=Path,help='prior mock-SDK test profiles only; restore test enrollment into the ephemeral local DB')
    ap.add_argument('--no-gpl',action='store_true',help='process-only existing renderer fallback; does not change the normal game config')
    ap.add_argument('--service-url',help='already-running wrangler local origin; numeric 127.0.0.1 HTTP only')
    a=ap.parse_args();assert Path(a.name).name==a.name
    out=ROOT/'build/run'/a.name;assert not out.exists();out.mkdir()
    binary=out/'bin';binary.mkdir();env=msvc_environment()
    # Read-only seed copy; no reset or writes to the normal game's cache.
    cache=out/'cache/vk';cache.mkdir(parents=True)
    for item in (ROOT/'build/cache/vk').iterdir():
        if item.is_file():shutil.copy2(item,cache/item.name)
    cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
    sys.path.insert(0,str(ROOT/'tools/lifter'));import build_exe as b
    localobj=out/'group_local.obj'
    def run(cmd):
        r=subprocess.run(cmd,cwd=out,env=env,capture_output=True,text=True)
        with (out/'build.log').open('a') as f:f.write(r.stdout+r.stderr)
        assert r.returncode==0,r.stdout+r.stderr
    run([str(cl),*b.msvc_flags(True),'/std:c11','/DSTRIKERS_GROUP_LOCAL_TEST=1','/Fo'+str(localobj),str(ROOT/'runtime/services/group_client.c')])
    objs=sorted((ROOT/'build/obj/host/opt').glob('*.obj'))
    objs=[localobj if p.name=='runtime_services_group_client.obj' else p for p in objs]
    manifest=json.loads((ROOT/'runtime/runtime_sources.json').read_text())
    run([str(cl.parent/'link.exe'),'/nologo','/OUT:'+str(binary/'strikers_native.exe'),*[str(p) for p in objs],str(ROOT/'build/bin/strikers_game.lib'),str(ROOT/'deps/SDL3/lib/x64/SDL3.lib'),*[x for x in manifest['host_libraries'] if x!='SDL3.lib']])
    mock=MOCK.replace('#include <string.h>','#include <string.h>\n#include <stdlib.h>')
    mock=mock.replace('return 0xabcdef0123ULL;','return _strtoui64(getenv("STRIKERS_TEST_ZT_NODE"),NULL,16);')
    mock=mock.replace('int zts_socket(int f,int t,int p){return -1;}','int zts_socket(int f,int t,int p){return 1;}')
    mock=mock.replace('int zts_bind(int fd,const char *p,unsigned short port){return -1;}','int zts_bind(int fd,const char *p,unsigned short port){return 0;}')
    mock=mock.replace('int zts_bsd_fcntl(int fd,int cmd,int flags){return -1;}','int zts_bsd_fcntl(int fd,int cmd,int flags){return 0;}')
    mock=mock.replace('zts_socklen_t l){return -1;}','zts_socklen_t l){return (ssize_t)n;}')
    source=out/'sdk_mock.c';source.write_text(mock)
    run([str(cl),'/nologo','/LD','/MD','/O2','/std:c11','/I'+str(ROOT/'deps/libzt/include'),str(source),'/Fe:'+str(binary/'strikers_zt.dll'),'/link','/EXPORT:zts_util_ipstr_to_saddr'])
    for name in ('SDL3.dll','strikers_game.dll','strikers_glslang.dll','libopus-0.dll'):
        shutil.copy2(ROOT/'build/bin'/name,binary/name)
    records=[];children=[];files=[];server=None
    base={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    try:
        f=(out/'server.log').open('wb');files.append(f)
        if not a.service_url:
            server=subprocess.Popen(['node','test/native-server.mjs'],cwd=ROOT/'cloudflare/group',stdout=f,stderr=subprocess.STDOUT)
            for _ in range(100):
                if 'READY' in (out/'server.log').read_text(errors='replace'):break
                assert server.poll() is None;time.sleep(.1)
        origin=a.service_url or 'http://127.0.0.1:8787'
        code=call('/admin/codes',{},origin)['code']
        for i,name in enumerate(('Alice','Bob')):
            profile=out/name; (profile/'save').mkdir(parents=True)
            if a.enrolled_from:
                fixture=a.enrolled_from.resolve();assert fixture.is_relative_to(ROOT/'build/run')
                encrypted=(fixture/name/'group-credential.dat').read_bytes()
                class Blob(ctypes.Structure):_fields_=[('size',ctypes.c_ulong),('data',ctypes.c_void_p)]
                raw=ctypes.create_string_buffer(encrypted);src=Blob(len(encrypted),ctypes.cast(raw,ctypes.c_void_p));plain=Blob()
                assert ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(src),None,None,None,None,1,ctypes.byref(plain))
                credential=ctypes.string_at(plain.data,plain.size)[264:329].split(b'\0')[0].decode()
                ctypes.windll.kernel32.LocalFree.argtypes=[ctypes.c_void_p];ctypes.windll.kernel32.LocalFree(plain.data)
                node=('aaaaaaaaaa','bbbbbbbbbb')[i]
                assert len(credential)==64
                call('/join',{'node':node,'name':name,'code':code,'credential':credential},origin)
                (profile/'group-credential.dat').write_bytes(encrypted)
            shutil.copy2(ROOT/'build/run/int02-qb-userdata/save/strkrs.save',profile/'save/strkrs.save')
            config=out/(name+'.ini');config.write_text(f'window=windowed\nwidth=1280\nheight=900\nresolution_scale=0.5\nunfocused=run\nvolume=0\n[Network]\nmode=zerotier\ngroup_service={origin}\ngroup_code={code}\n')
            # Host script is deliberately delayed so enrollment can be clicked first.
            seq=out/(name+'.seq')
            if i==0:
                lines=[]
                for line in (ROOT/'tools/audit/job18_lan_host.seq').read_text().splitlines():
                    if line.startswith('5100'):break
                    if line and line[0].isdigit():
                        frame,rest=line.split('+',1);lines.append(str(int(frame)+(0 if a.enrolled_from else 3000))+'+'+rest)
                seq.write_text('\n'.join(lines)+'\n')
            else:seq.write_text('# no game input; test the Online tab\n')
            shot=profile/'screens';shot.mkdir()
            e=dict(base,STRIKERS_PC_CONFIG=str(config),STRIKERS_USERDATA=str(profile),STRIKERS_NETWORK='zerotier',STRIKERS_CONFIG=str(out/(name+'-game.ini')),STRIKERS_ROMFS=str(ROOT/'build/dump/update/romfs'),STRIKERS_TEST_ZT_NODE=('aaaaaaaaaa','bbbbbbbbbb')[i],STRIKERS_PC_SHOT='0',STRIKERS_SHOT_DIR=str(shot),STRIKERS_VOLUME='0',STRIKERS_SDL_TEST_PLAYERS='1',STRIKERS_SDL_TEST_SEQ=str(seq),STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_LDN_TRACE='1',STRIKERS_VK_CACHE=str(cache),STRIKERS_VK_WORKERS='2')
            if a.no_gpl:e['STRIKERS_VK_GPL']='0'
            f=(out/(name+'.log')).open('wb');files.append(f)
            p=subprocess.Popen([str(binary/'strikers_native.exe'),str(ROOT/'build/dump/update/elf/main.elf'),str(a.seconds)],cwd=binary,env=e,stdout=f,stderr=subprocess.STDOUT);children.append(p)
            records.append({'name':name,'pid':p.pid,'userdata':str(profile.relative_to(ROOT)),'config':str(config.relative_to(ROOT)),'executable_sha256':hashlib.sha256((binary/'strikers_native.exe').read_bytes()).hexdigest(),'sdk':'mock; discards datagrams','service':'externally started loopback service' if a.service_url else 'loopback production handler + SQLite fallback; mock Central','env':{k:v for k,v in e.items() if k.startswith('STRIKERS_')}})
        (out/'manifest.json').write_text(json.dumps(records,indent=2));print(json.dumps(records,indent=2),flush=True)
        deadline=time.monotonic()+a.seconds
        while time.monotonic()<deadline and any(p.poll() is None for p in children):time.sleep(1)
    finally:
        for p in children:
            if p.poll() is None:
                subprocess.run([sys.executable,str(ROOT/'tools/audit/pc_window_probe.py'),str(p.pid),'close'],capture_output=True)
                try:p.wait(timeout=15)
                except subprocess.TimeoutExpired:p.kill();p.wait()
        for record,p in zip(records,children):record['exit_code']=p.returncode
        (out/'manifest.json').write_text(json.dumps(records,indent=2))
        if server is not None:server.terminate();server.wait()
        for f in files:f.close()

if __name__=='__main__':main()
