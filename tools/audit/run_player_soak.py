"""Continuous Quick Battle replay in an isolated profile, sampled every minute.

Uses ordinary SDL replay input and save-event fences; no guest memory writes.
Does not equate staying alive in menus with completed back-to-back matches.
"""
import argparse
import csv
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
from types import SimpleNamespace

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import sha256, within, clean_environment


def sequence(repeats,seeded=False):
    original=(ROOT/'tools/audit/job11_quick_battle.seq').read_text()
    # Four-frame direction taps can be missed by the asynchronous SDL/HID path
    # on this machine. Keep them below menu auto-repeat, but hold for 12 frames.
    original=original.replace('+4:', '+12:')
    # The actual current game menu accepts the left stick for team selection;
    # D-pad taps left the controller unassigned. Use the normal analog boundary.
    original=re.sub(r'(?<=:)[JIKL]',lambda m:dict(J='l',I='u',K='d',L='r')[m[0]],original)
    before,after=original.split('wait open:StrkrsSave:/strkrs.save')
    menu=[]
    for line in before.splitlines():
        m=re.fullmatch(r'(\d+)(\+\d+:.+)',line)
        if m and int(m[1])>=2100:
            # Each repeated block starts after the previous result-screen fence.
            menu.append(str(int(m[1])-2100+2800)+m[2])
    if seeded:
        # Saved profiles skip the first-run tutorial/online notices. Reusing the
        # fresh-profile key presses would navigate past Quick Battle.
        first='300+12:Z\n800+8:Z\n1000+8:*\n2000+8:*\n'
        first+='\n'.join(line for line in before.splitlines()
                         if re.match(r'\d+\+',line) and int(line.split('+')[0])>=2100)
        result=first+'\nwait open:StrkrsSave:/strkrs.save'+after
    else:result=original
    for _ in range(repeats-1):
        result+='\n# Next match, from main menu (same process)\n'+'\n'.join(menu)
        result+='\nwait open:StrkrsSave:/strkrs.save'+after
    if len([s for s in result.splitlines() if s and not s.startswith('#')])>1024:
        raise ValueError('Replay exceeds the production input parser limit')
    return result


def metrics(process):
    class Counters(C.Structure):
        _fields_=[('cb',W.DWORD),('PageFaultCount',W.DWORD)]+[(name,C.c_size_t) for name in
            ('PeakWorkingSetSize','WorkingSetSize','QuotaPeakPagedPoolUsage','QuotaPagedPoolUsage',
             'QuotaPeakNonPagedPoolUsage','QuotaNonPagedPoolUsage','PagefileUsage','PeakPagefileUsage','PrivateUsage')]
    kernel=C.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
    kernel.GetProcessHandleCount.argtypes=[W.HANDLE,C.POINTER(W.DWORD)]
    kernel.CloseHandle.argtypes=[W.HANDLE]
    psapi=C.WinDLL('psapi',use_last_error=True)
    psapi.GetProcessMemoryInfo.argtypes=[W.HANDLE,C.c_void_p,W.DWORD]
    handle=kernel.OpenProcess(0x410,False,process.pid)
    if not handle: raise C.WinError(C.get_last_error())
    try:
        counters=Counters();counters.cb=C.sizeof(counters);handles=W.DWORD()
        if not psapi.GetProcessMemoryInfo(handle,C.byref(counters),counters.cb):raise C.WinError(C.get_last_error())
        if not kernel.GetProcessHandleCount(handle,C.byref(handles)):raise C.WinError(C.get_last_error())
        # Toolhelp thread snapshots require no CIM/WMI or elevated permissions.
        class Thread(C.Structure):
            _fields_=[('size',W.DWORD),('usage',W.DWORD),('id',W.DWORD),('owner',W.DWORD),
                      ('base',W.LONG),('delta',W.LONG),('flags',W.DWORD)]
        kernel.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];kernel.CreateToolhelp32Snapshot.restype=W.HANDLE
        kernel.Thread32First.argtypes=[W.HANDLE,C.POINTER(Thread)]
        kernel.Thread32Next.argtypes=[W.HANDLE,C.POINTER(Thread)]
        snap=kernel.CreateToolhelp32Snapshot(4,0)
        if snap==C.c_void_p(-1).value:raise C.WinError(C.get_last_error())
        thread=Thread();thread.size=C.sizeof(thread);count=0
        try:
            ok=kernel.Thread32First(snap,C.byref(thread))
            while ok:
                count+=int(thread.owner==process.pid)
                ok=kernel.Thread32Next(snap,C.byref(thread))
        finally:kernel.CloseHandle(snap)
        return dict(working_set=counters.WorkingSetSize,private_bytes=counters.PrivateUsage,
                    handles=handles.value,threads=count)
    finally:kernel.CloseHandle(handle)


def slope(rows,key):
    x=[r['seconds']/3600 for r in rows]; y=[r[key] for r in rows]
    if len(x)<2:return None
    mx=sum(x)/len(x);my=sum(y)/len(y)
    return sum((a-mx)*(b-my) for a,b in zip(x,y))/sum((a-mx)**2 for a in x)


def match_fences(script,log):
    """Only match-end save fences count; menu/movie waits are not matches."""
    steps=[line.strip() for line in script.splitlines() if line.strip() and not line.lstrip().startswith('#')]
    indices={i+1 for i,line in enumerate(steps) if line=='wait open:StrkrsSave:/strkrs.save'}
    return sum(int(block) in indices for block in re.findall(r'^input wait: satisfied at frame \d+; block (\d+) begins',log,re.M))


def webm_handles(row,census,log):
    """Audit 06 allowance applies only to Event handles, two per opened WebM."""
    opens=len(re.findall(r'^movie: opened [^\r\n]+\.webm:',log,re.M|re.I))
    events=census['types'].get('Event',0)
    return dict(row,webm_opens=opens,event_handles=events,
                non_event_handles=census['total']-events,
                adjusted_event_handles=events-2*opens,
                adjusted_handles=row['handles']-2*opens)


def growth_report(rows,seconds,allow_known_webm_events=False):
    # A normal close can leave a final sample just beyond the requested run.
    # Select the actual elapsed-time window, not half of an incidental row count.
    stable=[r for r in rows if seconds/2<=r['seconds']<seconds]
    trends={k:slope(stable,k) for k in ('private_bytes','handles','threads')}
    enough=len(stable)>=30
    no_growth=(enough and trends['private_bytes']<=16*1024*1024 and trends['handles']<=5 and trends['threads']<=1)
    report=dict(last_half_slope_per_hour=trends,
        growth_window=dict(start_seconds=seconds/2,end_seconds_exclusive=seconds,samples=len(stable),
            excluded_at_or_after_end=[r['seconds'] for r in rows if r['seconds']>=seconds]),
        no_growth_thresholds=dict(private_bytes_per_hour=16*1024*1024,handles_per_hour=5,threads_per_hour=1))
    if allow_known_webm_events:
        residual={k:slope(stable,k) for k in ('adjusted_handles','non_event_handles','adjusted_event_handles','event_handles','webm_opens')}
        report['last_half_handle_slopes_per_hour']=residual
        # The allowance cannot mask growth in another handle category.
        no_growth=(enough and trends['private_bytes']<=16*1024*1024 and trends['threads']<=1 and
            all(residual[k]<=5 for k in ('adjusted_handles','non_event_handles','adjusted_event_handles')))
    return report,no_growth


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--dll',type=Path,required=True)
    p.add_argument('--name',required=True)
    p.add_argument('--seconds',type=int,default=7200)
    p.add_argument('--no-gpl',action='store_true',help='process-only renderer workaround, recorded explicitly')
    p.add_argument('--seed-save',type=Path,help='copy only this intro-complete test save; never modify its original')
    p.add_argument('--host',type=Path,default=ROOT/'build/bin/strikers_native.exe')
    p.add_argument('--sdk',type=Path,help='locally generated sidecar required by the release-host candidate')
    p.add_argument('--sequence',type=Path,help='explicit ordinary HID replay for a cold-start acceptance run')
    p.add_argument('--runtime-dir',type=Path,help='prepared app-local CRT DLLs; never installs a system runtime')
    p.add_argument('--clean-env',action='store_true',help='allowlisted profile/environment with only System32 on PATH')
    p.add_argument('--launcher',type=Path,help='exercise the real launcher and its message-loop self-test')
    p.add_argument('--fslog',action='store_true',help='record all successful guest file-open events for replay design')
    p.add_argument('--wait-idle',action='store_true',help='require two quiet minutes and invalidate an overlapping game run')
    p.add_argument('--handle-log',action='store_true',help='enable audit 06 thread/movie handle checkpoints')
    p.add_argument('--allow-known-webm-events',action='store_true',help='allow only the evidenced Windows leak of two Event handles per successful WebM open')
    p.add_argument('--handle-census',type=Path,help='native read-only handle-type probe, required for the WebM allowance')
    p.add_argument('--romfs',type=Path,default=ROOT/'build/dump/update/romfs')
    p.add_argument('--elf',type=Path,default=ROOT/'build/dump/update/elf/main.elf')
    a=p.parse_args()
    if a.allow_known_webm_events and (not a.handle_log or not a.handle_census or not a.handle_census.is_file()):
        p.error('WebM allowance requires --handle-log and an existing --handle-census executable')
    if a.wait_idle:
        from job21b_launch import strikers_processes,process_snapshot
        quiet=None
        while True:
            others=strikers_processes()
            if others:quiet=None
            elif quiet is None:quiet=time.monotonic()
            if quiet is not None and time.monotonic()-quiet>=120:break
            print('Waiting for quiet PC: '+json.dumps(others),flush=True);time.sleep(5)
    out=within(ROOT/'build/phase8',ROOT/'build/phase8'/a.name)
    out.mkdir(parents=True,exist_ok=False)
    bin=out/'bin';bin.mkdir()
    for name in ('strikers_glslang.dll','strikers_zt.dll','libopus-0.dll','SDL3.dll'):
        shutil.copyfile(ROOT/'build/bin'/name,bin/name)
    shutil.copyfile(a.host,bin/'strikers_native.exe')
    shutil.copyfile(a.dll,bin/'strikers_game.dll')
    if a.launcher:shutil.copyfile(a.launcher,bin/'StrikersRecomp.exe')
    if a.sdk:shutil.copyfile(a.sdk,bin/'strikers_sdk.dll')
    if a.runtime_dir:
        for path in a.runtime_dir.glob('*.dll'):shutil.copyfile(path,bin/path.name)
    if a.sequence:shutil.copyfile(a.sequence,out/'replay.seq')
    else:(out/'replay.seq').write_text(sequence(16,a.seed_save is not None))
    userdata=bin/'userdata' if a.launcher else out/'userdata'
    if a.seed_save:
        (userdata/'save').mkdir(parents=True)
        shutil.copyfile(a.seed_save,userdata/'save/strkrs.save')
    (out/'strikers.ini').write_text('window=windowed\nwidth=1280\nheight=720\nresolution_scale=1\nupscale=linear\n[Network]\nmode=offline\n')
    if a.clean_env:
        env=clean_environment(out,ROOT/'toolchain/llvm')
        env['PATH']=str(Path(env['SystemRoot'])/'System32')
        env.pop('LLVM_ROOT',None)
    else:env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_ROMFS=a.romfs.resolve().as_posix(),STRIKERS_USERDATA=userdata.as_posix(),
               STRIKERS_CONFIG=((userdata/'controllers.ini') if a.launcher else out/'controllers.ini').as_posix(),
               STRIKERS_PC_CONFIG=((bin/'strikers.ini') if a.launcher else out/'strikers.ini').as_posix(),
               STRIKERS_SDL_TEST_SEQ=(out/'replay.seq').as_posix(),STRIKERS_KEYBOARD_PLAYER='0',
               STRIKERS_VOLUME='0',STRIKERS_SHOW='0',STRIKERS_TRACE=(out/'boot_trace.txt').as_posix(),STRIKERS_NETWORK='offline')
    if a.no_gpl:env['STRIKERS_VK_GPL']='0'
    if a.fslog:env['STRIKERS_FSLOG']='1'
    if a.handle_log:env['STRIKERS_HANDLE_LOG']='1'
    command=[str(bin/'strikers_native.exe'),str(a.elf.resolve()),str(a.seconds)]
    game_log=out/'game.log'
    if a.launcher:
        shutil.copyfile(out/'strikers.ini',bin/'strikers.ini')
        test=out/'launcher-test.seq'
        test.write_text(f'snapshot launcher-start\nduration {a.seconds}\nelf {a.elf.resolve()}\nromfs {a.romfs.resolve()}\nclick\nwait started\nsnapshot launcher-running\nwait done\nexpect success\nexit\n',encoding='utf-8')
        command=[str(bin/'StrikersRecomp.exe'),'--self-test',str(test)]
        game_log=bin/'userdata/last-game.log'
    manifest=dict(seconds_requested=a.seconds,host_sha256=sha256(bin/'strikers_native.exe'),
                  game_sha256=sha256(bin/'strikers_game.dll'),env={k:v for k,v in env.items() if k.startswith('STRIKERS_')})
    manifest['binary_sha256']={p.name:sha256(p) for p in bin.iterdir() if p.suffix.lower() in ('.exe','.dll')}
    manifest['clean_environment']=env if a.clean_env else None
    manifest['game_log']=str(game_log.resolve())
    if a.allow_known_webm_events:
        manifest['known_windows_webm_allowance']=dict(handle_type='Event',handles_per_open=2,
            basis='docs/audit-06.md F74, standalone Media Foundation source probe',
            census_sha256=sha256(a.handle_census),census_source_sha256=sha256(ROOT/'tools/audit/player_handle_census.cpp'))
    if a.seed_save:manifest['seed_save_sha256']=sha256(a.seed_save)
    start=time.monotonic();rows=[];contamination=[]
    with (out/'game.log').open('wb') as log:
        child=subprocess.Popen(command,
                               cwd=bin,env=env,stdout=log,stderr=subprocess.STDOUT)
        manifest['pid']=child.pid
        observed=child
        if a.launcher:
            until=time.monotonic()+30
            state=bin/'launcher-child.json'
            while not state.exists() and child.poll() is None and time.monotonic()<until:time.sleep(.2)
            if not state.exists():raise RuntimeError('Launcher did not report its owned game process')
            observed=SimpleNamespace(pid=json.loads(state.read_text())['pid'])
            owned=[q for q in process_snapshot() if q['pid']==observed.pid and q['parent']==child.pid and q['name'].lower()=='strikers_native.exe']
            if len(owned)!=1:raise RuntimeError('Launcher child ownership not verified')
            manifest['launcher_pid']=child.pid;manifest['pid']=observed.pid
        (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
        with (out/'resources.csv').open('w',newline='') as f:
            fields=['seconds','working_set','private_bytes','handles','threads']
            if a.allow_known_webm_events:fields+=['webm_opens','event_handles','non_event_handles','adjusted_event_handles','adjusted_handles']
            writer=csv.DictWriter(f,fieldnames=fields)
            writer.writeheader()
            try:
                next_sample=start;next_process_check=start
                while child.poll() is None:
                    now=time.monotonic()
                    if a.wait_idle and now>=next_process_check:
                        contamination=[q for q in strikers_processes() if q['pid']!=observed.pid]
                        next_process_check=now+2
                        if contamination:
                            from prepare_graphics import close_owned_window
                            owned=[q for q in process_snapshot() if q['pid']==observed.pid and q['name'].lower()=='strikers_native.exe' and (not a.launcher or q['parent']==child.pid)]
                            if len(owned)==1:close_owned_window(observed.pid)
                            try:child.wait(60)
                            except subprocess.TimeoutExpired:child.kill()
                            print('CONTAMINATED: '+json.dumps(contamination),flush=True);break
                    if now>=next_sample:
                        current_log=game_log.read_text(errors='replace') if game_log.exists() else ''
                        try: row=dict(seconds=round(now-start,2),**metrics(observed))
                        except OSError:
                            if child.poll() is not None:break
                            raise
                        if a.allow_known_webm_events:
                            probe=subprocess.run([str(a.handle_census.resolve()),str(observed.pid),str((bin/'strikers_native.exe').resolve())],
                                capture_output=True,text=True,check=True,timeout=15)
                            census=json.loads(probe.stdout)
                            if manifest.get('census_created_filetime',census['created_filetime'])!=census['created_filetime']:
                                raise RuntimeError('Owned process creation time changed')
                            manifest['census_created_filetime']=census['created_filetime']
                            with (out/'handle-types.jsonl').open('a') as types:
                                types.write(json.dumps(dict(seconds=row['seconds'],**census))+'\n')
                            row=webm_handles(row,census,current_log)
                        rows.append(row);writer.writerow(row);f.flush()
                        print(json.dumps(dict(row,match_end_fences=match_fences((out/'replay.seq').read_text(),current_log))),flush=True);next_sample+=60
                    if now-start>a.seconds+45:
                        child.kill();manifest['forced_timeout']=True;break
                    time.sleep(1)
            finally:
                if child.poll() is None:child.kill()
                child.wait()
    if a.launcher:shutil.copyfile(game_log,out/'game.log')
    text=(out/'game.log').read_text(errors='replace')
    completed=match_fences((out/'replay.seq').read_text(),text)
    errors=re.findall(r'^.*(?:GUEST TRAP|UNTRANSLATED INSTRUCTION|unhandled exception|VUID-|pipeline.*failed).*$' ,text,re.M|re.I)
    growth,no_growth=growth_report(rows,a.seconds,a.allow_known_webm_events)
    manifest.update(growth)
    manifest.update(exit=child.returncode,wall_seconds=time.monotonic()-start,samples=len(rows),
                    match_end_fences=completed,errors=errors[:25],
                    valid_timing=not contamination,contamination=contamination,
                    status='CONTAMINATED' if contamination else 'PASS' if child.returncode==0 and a.seconds>=7200 and len(rows)>=120 and completed>=10 and no_growth and not errors else 'NOT_PASSED')
    (out/'result.json').write_text(json.dumps(manifest,indent=2))
    print(json.dumps(manifest,indent=2))
    return 0 if manifest['status']=='PASS' else 1


if __name__=='__main__':raise SystemExit(main())
