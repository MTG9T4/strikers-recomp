"""Serial, isolated launch measurements; no shared cache deletion or guest writes."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools/release'),str(ROOT/'installer'),str(ROOT/'tools/audit')]
from common import clean_environment,sha256,within
from prepare_graphics import close_owned_window


def process_snapshot():
    class Entry(C.Structure):
        _fields_=[('size',W.DWORD),('usage',W.DWORD),('pid',W.DWORD),('heap',C.c_size_t),
            ('module',W.DWORD),('threads',W.DWORD),('parent',W.DWORD),('priority',W.LONG),
            ('flags',W.DWORD),('exe',W.WCHAR*260)]
    k=C.WinDLL('kernel32',use_last_error=True)
    k.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];k.CreateToolhelp32Snapshot.restype=W.HANDLE
    k.Process32FirstW.argtypes=[W.HANDLE,C.POINTER(Entry)];k.Process32NextW.argtypes=[W.HANDLE,C.POINTER(Entry)]
    k.CloseHandle.argtypes=[W.HANDLE]
    handle=k.CreateToolhelp32Snapshot(2,0)
    if handle==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
    result=[];entry=Entry();entry.size=C.sizeof(entry)
    try:
        ok=k.Process32FirstW(handle,C.byref(entry))
        while ok:
            result.append(dict(pid=entry.pid,parent=entry.parent,name=entry.exe))
            ok=k.Process32NextW(handle,C.byref(entry))
    finally:k.CloseHandle(handle)
    return result


def strikers_processes():
    return [p for p in process_snapshot() if p['name'].lower().startswith(('strikers','eden','yuzu','ryujinx'))]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name',required=True);p.add_argument('--install',type=Path)
    p.add_argument('--sequence',type=Path);p.add_argument('--seconds',type=int,default=3600)
    p.add_argument('--stop-frame',type=int,default=0)
    p.add_argument('--elf',type=Path)
    p.add_argument('--romfs',type=Path)
    p.add_argument('--diagnostics',action='store_true')
    p.add_argument('--stop-matches',type=int,default=0)
    p.add_argument('--post-match-frames',type=int,default=2400)
    p.add_argument('--wait-idle',action='store_true')
    p.add_argument('--idle-seconds',type=int,default=120,help='require a quiet interval before an uncontended run')
    p.add_argument('--wrapper',action='append',default=[],help='a program the host runs under (audit 07: a debugger loop)')
    p.add_argument('--env',action='append',default=[],help='KEY=VALUE for the host (audit 07 experiments)')
    a=p.parse_args();out=within(ROOT/'build/phase8',ROOT/'build/phase8'/a.name);out.mkdir(exist_ok=False)
    tasklist=subprocess.run(['tasklist','/FI','IMAGENAME eq strikers*'],capture_output=True,text=True)
    (out/'tasklist.txt').write_text(tasklist.stdout+tasklist.stderr)
    idle_since=None
    while True:
        others=strikers_processes()
        if not others:
            if idle_since is None:idle_since=time.monotonic()
            if time.monotonic()-idle_since>=a.idle_seconds:break
            print('Waiting for a quiet process interval',flush=True);time.sleep(5);continue
        idle_since=None
        if not a.wait_idle:raise RuntimeError('Other Strikers processes are running: '+str(others))
        print('Waiting for other Strikers processes: '+json.dumps(others),flush=True);time.sleep(30)
    install=a.install.resolve() if a.install else None
    host=install/'strikers_native.exe' if install else ROOT/'build/bin/strikers_native.exe'
    cwd=install or out
    cache=cwd/'cache/vk'
    before=dict(exists=cache.exists(),files=sum(1 for f in cache.rglob('*') if f.is_file()) if cache.exists() else 0)
    seq=out/'replay.seq'
    if a.sequence:shutil.copyfile(a.sequence,seq)
    else:
        seq.write_text('wait movie\n300+12:Z\n600+8:*\n800+12:Z\n1000+12:Z\n1350+12:Z\n1550+12:d\n1650+12:Z\nwait movie\n300+8:*\n')
    env=clean_environment(out/'sandbox',ROOT/'build/phase8/portable2/toolchain/llvm')
    env.update(STRIKERS_VK_CACHE='cache/vk',STRIKERS_VK_FRAMELOG=f'1-{max(60000,a.seconds*120)}',STRIKERS_FSLOG='1',
        STRIKERS_ROMFS=str(a.romfs.resolve() if a.romfs else (install or ROOT)/'build/dump/update/romfs'),STRIKERS_USERDATA=str(out/'userdata'),
        STRIKERS_PC_CONFIG=str(out/'strikers.ini'),STRIKERS_CONFIG=str(out/'controllers.ini'),
        STRIKERS_SDL_TEST_SEQ=str(seq),STRIKERS_VOLUME='0',STRIKERS_NETWORK='offline',STRIKERS_SHOW='0')
    env.update(e.split('=',1) for e in a.env)
    (out/'strikers.ini').write_text('window=windowed\nwidth=1280\nheight=720\nresolution_scale=1\nupscale=linear\n[Network]\nmode=offline\n')
    elf=a.elf.resolve() if a.elf else (install or ROOT)/'build/dump/update/elf/main.elf'
    command=[*a.wrapper,str(host),str(elf),str(a.seconds)];game_log=out/'driver.log'
    if install:
        script=out/'launcher.seq'
        script.write_text(f'snapshot start\nduration {a.seconds}\nelf {elf}\nromfs {env["STRIKERS_ROMFS"]}\nclick\nwait started\nsnapshot preparing\nwait done\nsnapshot done\nexpect success\nexit\n')
        command=[str(install/'StrikersRecomp.exe'),'--self-test',str(script)]
        game_log=install/'userdata/last-game.log'
    manifest=dict(host_sha256=sha256(host),cache_before=before,command=command,env=env,
        no_other_strikers_at_start=True,tasklist_exit=tasklist.returncode)
    manifest['binary_sha256']={p.name:sha256(p) for p in host.parent.iterdir() if p.suffix.lower() in ('.exe','.dll')}
    steps=[s.strip() for s in seq.read_text().splitlines() if s.strip() and not s.lstrip().startswith('#')]
    match_blocks={i+1 for i,s in enumerate(steps) if s=='wait open:StrkrsSave:/strkrs.save'}
    start=time.monotonic();offset=0;pending='';events=[];last_frame=0;close_at=None;match_count=0;match_frame=0
    state=install/'launcher-child.json' if install else None
    prior_state=state.stat().st_mtime_ns if state and state.exists() else None
    launched=not install
    with (out/'driver.log').open('wb') as output,(out/'events.jsonl').open('w') as timeline:
        child=subprocess.Popen(command,cwd=cwd,env=env,stdout=output,stderr=subprocess.STDOUT,
            creationflags=subprocess.CREATE_NO_WINDOW)
        pid=child.pid
        try:
            next_status=start;next_process_check=start;next_diagnostics=start;contamination=[]
            while child.poll() is None:
                now=time.monotonic()
                if state and not launched and state.exists() and state.stat().st_mtime_ns!=prior_state:
                    candidate=json.loads(state.read_text())['pid']
                    owned=[p['pid'] for p in process_snapshot() if p['parent']==child.pid and p['name'].lower()=='strikers_native.exe']
                    # A separately launched game can replace the shared state
                    # file. Bind once to this launcher's actual child only.
                    if candidate in owned:pid=candidate;launched=True
                    elif len(owned)==1:pid=owned[0];launched=True
                if launched and game_log.exists():
                    with game_log.open('r',encoding='utf-8',errors='replace') as log:
                        log.seek(offset);text=log.read();offset=log.tell()
                    pending+=text
                    parts=pending.split('\n');pending=parts.pop()
                    for line in parts:
                        match=re.search(r'nvn_vk_frame: (\d+) draws',line)
                        if match:last_frame=max(last_frame,int(match[1]))
                        fence=re.search(r'input wait: satisfied at frame (\d+); block (\d+) begins',line)
                        if fence and int(fence[2]) in match_blocks:
                            match_count+=1;match_frame=int(fence[1])
                        frame=re.search(r'(?:nvn_vk_frame: |capture-frame: )(\d+)',line)
                        if frame:last_frame=max(last_frame,int(frame[1]))
                        if (match and int(match[1])==1) or any(mark in line for mark in
                            ('input wait:','movie: opened ','capture-frame:','picture saved','first play frame')):
                            entry=dict(seconds=round(now-start,3),line=line);events.append(entry)
                            timeline.write(json.dumps(entry)+'\n');timeline.flush();print(json.dumps(entry),flush=True)
                finished=(a.stop_frame and last_frame>=a.stop_frame) or (a.stop_matches and match_count>=a.stop_matches and last_frame>=match_frame+a.post_match_frames)
                if finished and close_at is None:
                    close_owned_window(pid);close_at=now
                if launched and now>=next_process_check:
                    contamination=[q for q in strikers_processes() if q['pid']!=pid and q['parent']!=pid]
                    if contamination:
                        print('CONTAMINATED: '+json.dumps(contamination),flush=True);break
                    next_process_check=now+2
                if a.diagnostics and launched and now>=next_diagnostics:
                    from job21c_diagnostics import snapshot
                    with (out/'thread-samples.jsonl').open('a') as samples:
                        samples.write(json.dumps(dict(seconds=round(now-start,3),**snapshot(pid)))+'\n')
                    next_diagnostics=now+15
                if close_at and now-close_at>60:raise RuntimeError('Owned host did not close')
                if now-start>a.seconds+60:raise RuntimeError('Launch measurement timeout')
                if now>=next_status:
                    print(json.dumps(dict(seconds=round(now-start,1),frame=last_frame,pid=pid)),flush=True);next_status+=60
                time.sleep(.1)
        finally:
            if child.poll() is None:
                close_owned_window(pid)
                try:child.wait(15)
                except subprocess.TimeoutExpired:child.kill();child.wait()
    if install:shutil.copyfile(game_log,out/'game.log')
    targets_met=(not a.stop_matches or match_count>=a.stop_matches) and (not a.stop_frame or last_frame>=a.stop_frame)
    manifest.update(exit=child.returncode,seconds=time.monotonic()-start,last_frame=last_frame,events=events,
        match_end_fences=match_count,
        acceptance_targets_met=targets_met,
        valid_timing=not contamination,contamination=contamination,
        status='CONTAMINATED' if contamination else 'COMPLETED' if child.returncode==0 and targets_met else 'FAILED')
    manifest['cache_after']={str(p.relative_to(cache)):p.stat().st_size for p in cache.rglob('*') if p.is_file()}
    (out/'result.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps(dict(exit=child.returncode,seconds=manifest['seconds'],last_frame=last_frame)),flush=True)
    return 0 if manifest['status']=='COMPLETED' else 1


if __name__=='__main__':raise SystemExit(main())
