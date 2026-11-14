"""Owned real-SDK game pair for F1 or native self-test acceptance.

Only a numeric-loopback Worker and ZeroTier public roots are used. No guest
memory writes, mock SDK, or Central calls. Native mode calls the normal
group API explicitly; manual mode leaves enrollment to F1.
The outsider alone uses manual ad-hoc discovery hints to attack the real room.
"""
import argparse
import ctypes as C
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from urllib.parse import urlsplit

from job21b_launch import process_snapshot,close_owned_window
from test_group_service import call as local_call
import win_post
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import sha256


ORGANIZER=None


def call(path,data,origin):
    """Organizer call: the local fixture, or the real Worker with the token kept out of argv and logs."""
    if not ORGANIZER:return local_call(path,data,origin)
    # curl, because it verifies through the Windows store as the game's WinHTTP does; OpenSSL here picks an expired root.
    config=f'header = "Authorization: Bearer {ORGANIZER}"\nheader = "Content-Type: application/json"\nsilent\nfail-with-body\nmax-time = 20\n'
    if data is not None:config+='data = "'+json.dumps(data).replace('"','\\"')+'"\n'
    r=subprocess.run(['curl','-K','-',origin.rstrip('/')+path],input=config.encode(),capture_output=True,check=True)
    return json.loads(r.stdout)


FORM={'name':None,'join':None,'invite':None}


def wait_window(child,seconds=180):
    """The game process this launcher started, and its window."""
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        pids=[p['pid'] for p in process_snapshot() if p['parent']==child.pid and p['name'].lower()=='strikers_native.exe']
        if len(pids)==1:
            h=win_post.window(pids[0])
            if h:return pids[0],h
        time.sleep(.5)
    raise RuntimeError('game window did not appear')


def f1_enroll(folder,env,install,name):
    """Join through the real F1 form on the unmodified host, then close normally. The credential stays in userdata."""
    base={k:v for k,v in env.items() if k not in ('STRIKERS_SDL_TEST_SEQ','STRIKERS_REC','STRIKERS_TRACE','STRIKERS_SDL_TEST_PLAYERS','STRIKERS_KEYBOARD_PLAYER')}
    test=folder/'enroll.seq'
    test.write_text(f'duration 240\nelf {install}/build/dump/update/elf/main.elf\nromfs {install}/build/dump/update/romfs\nclick\nwait started\nwait done\nexpect success\nexit\n')
    log=folder/'userdata/last-game.log'
    with (folder/'enroll-driver.log').open('wb') as out:
        child=subprocess.Popen([str(folder/'StrikersRecomp.exe'),'--self-test',str(test)],cwd=folder,env=base,stdout=out,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        pid,h=wait_window(child)
        try:
            win_post.pin(h)
            time.sleep(12)
            win_post.key(h,win_post.VK_F1);time.sleep(2)
            win_post.shot(h,folder/'f1-form-empty.png')
            if not FORM['name']:return False
            win_post.click(h,*FORM['name']);time.sleep(.5);win_post.text(h,name);time.sleep(.8)
            win_post.shot(h,folder/'f1-form-filled.png')
            win_post.click(h,*FORM['join'])
            end=time.monotonic()+60
            while time.monotonic()<end and '[group] enrolled via configured service' not in (log.read_text(errors='replace') if log.exists() else ''):time.sleep(1)
            time.sleep(17)  # one presence poll, so the friends list is on screen
            win_post.shot(h,folder/'f1-joined.png')
            win_post.key(h,win_post.VK_F1);time.sleep(1)
            return '[group] enrolled via configured service' in log.read_text(errors='replace')
        finally:
            close_owned_window(pid)
            child.wait(90)
            if log.exists():shutil.copyfile(log,folder/'enroll-game.log')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--name',required=True)
    ap.add_argument('--install',type=Path,required=True)
    ap.add_argument('--host',type=Path,required=True)
    ap.add_argument('--service-url',required=True)
    ap.add_argument('--service-kind',choices=('workerd','Node SQLite fallback','Cloudflare production'),required=True)
    ap.add_argument('--relay',action='store_true')
    ap.add_argument('--outsider',action='store_true')
    ap.add_argument('--self-test',action='store_true',help='use the explicitly authorized native API/replay route, without desktop automation')
    ap.add_argument('--seconds',type=int,default=1800)
    ap.add_argument('--f1',action='store_true',help='exact player host: join and invite through the real F1 form with posted window messages')
    ap.add_argument('--organizer-file',type=Path,help='file holding ADMIN_TOKEN=, for the real HTTPS Worker')
    ap.add_argument('--form',help='name_x,name_y,join_x,join_y,invite_x,invite_y in the 1280x900 window')
    ap.add_argument('--enroll-only',action='store_true',help='stop after the F1 pass, to measure the form')
    ap.add_argument('--station-game-dll',type=Path,help='a different strikers_game.dll for the station (job 30: mixed CPU-level builds)')
    a=ap.parse_args()
    global ORGANIZER
    if a.form:
        v=[int(x) for x in a.form.split(',')];FORM.update(name=v[0:2],join=v[2:4],invite=v[4:6])
    assert Path(a.name).name==a.name
    url=urlsplit(a.service_url)
    if a.organizer_file:
        assert url.scheme=='https' and url.hostname.endswith('.workers.dev') and url.path in ('','/') and not url.query
        ORGANIZER=re.search(r'^ADMIN_TOKEN=(.+)$',a.organizer_file.read_text(),re.M)[1].strip()
    else:assert url.scheme=='http' and url.hostname=='127.0.0.1' and not url.username and not url.password and url.path in ('','/') and not url.query and not url.fragment
    install=a.install.resolve();host=a.host.resolve()
    assert install.is_relative_to(ROOT/'build/phase8') and host.is_relative_to(ROOT/'build')
    out=ROOT/'build/run'/a.name;out.mkdir(exist_ok=False)
    quiet=None
    while quiet is None or time.monotonic()-quiet<120:
        others=[p for p in process_snapshot() if p['name'].lower().startswith(('strikers','eden','yuzu','ryujinx'))]
        if others:quiet=None
        elif quiet is None:quiet=time.monotonic()
        print('Waiting for quiet PC: '+json.dumps(others),flush=True);time.sleep(5)
    roles=['host','station']+(['outsider'] if a.outsider else [])
    lib=C.CDLL(str(install/'strikers_zt.dll'))
    ids={};keys={}
    for role in roles:
        key=C.create_string_buffer(384);size=C.c_uint(384)
        assert lib.zts_id_new(key,C.byref(size))==0
        keys[role]=key.value;ids[role]=key.value.decode().split(':')[0]
    code=call('/admin/codes',{},a.service_url)['code']
    records=[];children=[];starts=[];files=[];contamination=[];complete=[]
    try:
        for role in roles:
            folder=out/role;folder.mkdir()
            for p in install.iterdir():
                if p.suffix.lower() in ('.dll','.exe'):shutil.copyfile(p,folder/p.name)
            shutil.copyfile(host,folder/'strikers_native.exe')
            if role=='station' and a.station_game_dll:shutil.copyfile(a.station_game_dll,folder/'strikers_game.dll')
            shutil.copytree(install/'cache',folder/'cache')
            user=folder/'userdata';(user/'save').mkdir(parents=True)
            shutil.copyfile(install/'userdata/save/strkrs.save',user/'save/strkrs.save')
            (user/'zerotier').mkdir();(user/'zerotier/identity.secret').write_bytes(keys[role])
            member=role!='outsider'
            config='window=windowed\nwidth=1280\nheight=900\nresolution_scale=0.5\nupscale=linear\nunfocused=run\nvolume=0\n[Network]\nmode=zerotier\n'
            config+=('zerotier_network=\ngroup_service='+a.service_url+'\ngroup_code='+code+'\n') if member else 'zerotier_network=ff61286137000000\ngroup_service=\n'
            (folder/'strikers.ini').write_text(config)
            direction='r' if role=='host' else 'l'
            replay=folder/'replay.seq'
            replay.write_text('300+12:Z\n800+12:Z\n1000+8:*\n2100+12:Z\n2300+8:*\n'
                'wait open:rom:/StadiumGlobal/Persistent.data\n600+8:*\n900+8:*\n'
                f'2500+180:{direction}S\n2510+8:*\n2670+8:*\n2750+120:u\n2900+60:Z\n'
                f'3100+180:{direction}S\n3300+60:Z\n3600+8:*\n'
                'wait open:StrkrsSave:/strkrs.save\n120+8:*\n900+12:!\n1080+8:*\n1500+12:Z\n1680+8:*\n1800+12:Z\n1980+8:*\n2100+12:K\n2220+12:Z\n2460+8:*\n3000+8:*\n3600+8:*\n')
            if not member:replay.write_text('300+12:Z\n800+12:Z\n2100+12:Z\n2300+8:*\n'+
                ''.join(f'{frame}+8:*\n' for frame in range(6000,54001,3000)))
            if a.self_test or a.f1:
                # A known title/main-menu prefix, then the real Local Wireless
                # menus. Everything after room start uses the established full
                # match route. Direction taps reach the ordinary pad boundary.
                lines=['300+12:Z','800+12:Z','1000+8:*','2100+12:Z','2300+8:*',
                    '3200+12:Z','3500+8:*','3700+4:K','4000+12:Z','4300+8:*']
                if role=='host':lines+=['4500+4:V','4700+8:*','4900+4:J','5100+12:Z','5400+8:*','6000+8:*']
                else:lines+=['5600+12:C','5900+8:*']
                if member:
                    if role=='station':lines+=['6200+12:Z','6500+8:*']
                    text=(ROOT/('tools/audit/job18_lan_'+('host' if role=='host' else 'station')+'.seq')).read_text()
                    body=False
                    for line in text.splitlines():
                        m=re.fullmatch(r'(\d+)(\+\d+:.+)',line)
                        if m and int(m[1])==5100:body=True
                        if not body:continue
                        if m:line=str(int(m[1])+(3400 if int(m[1])>=5600 else 2000))+m[2]
                        if m and int(m[1])==5600:line='9000+30:'+('l' if role=='host' else 'r')
                        # Once the save fence is reached, offsets stay relative.
                        if line.startswith('wait open:'):
                            lines.append(line);body=False
                            lines.extend(text.split(line,1)[1].splitlines());break
                        lines.append(line)
                else:lines+=['6200+12:Z','6500+8:*']+[step for f in range(8000,54001,3000) for step in (f'{f}+12:C',f'{f+300}+8:*')]
                replay.write_text('\n'.join(lines)+'\n')
            env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
            env.update(STRIKERS_NETWORK='zerotier',STRIKERS_SHOW='1',STRIKERS_VOLUME='0',
                STRIKERS_SDL_TEST_PLAYERS='1',STRIKERS_KEYBOARD_PLAYER='0',STRIKERS_SDL_TEST_SEQ=str(replay),
                STRIKERS_REC=str(folder/'input.rec'),STRIKERS_LDN_TRACE='1',STRIKERS_IMPORT_COUNTS='1',
                STRIKERS_FSLOG='1',STRIKERS_VK_FRAMELOG='1-150000',STRIKERS_VK_WORKERS='2',
                STRIKERS_TRACE=str(folder/'trace.txt'),STRIKERS_USERDATA=str(user),STRIKERS_CONFIG=str(user/'controllers.ini'))
            if not member:env['STRIKERS_ZT_PEERS']=','.join(ids[r] for r in ('host','station'))
            if a.self_test:
                shots=folder/'selftest-shots';shots.mkdir()
                env.update(STRIKERS_GROUP_TEST_ROLE=role,STRIKERS_GROUP_TEST_NAME='Host23b' if role=='host' else 'Guest23b',
                           STRIKERS_PC_SHOT='0',STRIKERS_SHOT_DIR=str(shots))
            if member and a.f1:
                done=f1_enroll(folder,env,install,'Host24' if role=='host' else 'Guest24')
                if a.enroll_only:return 0
                assert done,'F1 enrollment failed for '+role
            if a.relay:env['STRIKERS_ZT_BLOCK_DIRECT']=','.join(ids[r] for r in roles if r!=role)
            test=folder/'launcher.seq'
            test.write_text(f'duration {a.seconds}\nelf {install}/build/dump/update/elf/main.elf\nromfs {install}/build/dump/update/romfs\nclick\nwait started\nwait done\nexpect success\nexit\n')
            starts.append((folder,test,env))
            records.append(dict(role=role,launcher_pid=None,pid=None,node=ids[role],group_member=member,
                group_network='ff61286137000000',service_url=a.service_url,service_kind=a.service_kind,
                log=str((user/'last-game.log').relative_to(ROOT)),env={k:v for k,v in env.items() if k.startswith('STRIKERS_')},
                binary_sha256={p.name:sha256(p) for p in folder.iterdir() if p.suffix.lower() in ('.dll','.exe')},
                player_host_sha256=sha256(install/'strikers_native.exe'),replay=replay.read_text()))
            records[-1]['enrollment_route']='native self-test via group_join' if a.self_test else 'F1 form'
            if a.f1:records[-1]['f1_automation']='posted window messages to the unmodified player host'
        # Every game starts together, after all F1 enrollment: the replays are frame-timed against each other.
        for row,(folder,test,env) in zip(records,starts):
            # The outsider's menu route is frame-timed too, and three cold starts at once make it miss.
            if not row['group_member']:time.sleep(45)
            log=(folder/'driver.log').open('wb');files.append(log)
            child=subprocess.Popen([str(folder/'StrikersRecomp.exe'),'--self-test',str(test)],cwd=folder,
                env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(child);row['launcher_pid']=child.pid
        start=time.monotonic();printed=0;observed=[];stalled=[];invited=None;toast_until=0;toast_n=0
        while any(p.poll() is None for p in children):
            processes=process_snapshot()
            for row,child in zip(records,children):
                if row['pid'] is None:
                    owned=[p['pid'] for p in processes if p['parent']==child.pid and p['name'].lower()=='strikers_native.exe']
                    if len(owned)==1:row['pid']=owned[0]
            owned={r['pid'] for r in records if r['pid']}|{p.pid for p in children}
            contamination=[p for p in processes if p['name'].lower().startswith(('strikers','eden','yuzu','ryujinx')) and p['pid'] not in owned]
            if contamination:break
            (out/'manifest.json').write_text(json.dumps(records,indent=2))
            complete=[]
            for row,child in zip(records,children):
                if not row['group_member']:continue
                path=ROOT/row['log'];text=path.read_text(errors='replace') if path.exists() else ''
                frames=[int(f) for f in re.findall(r'^\[group-selftest\] role=\w+ frame=(\d+)',text,re.M)]
                if a.self_test and frames and max(frames)>=15000 and 'open "rom:/StadiumGlobal/Persistent.data"' not in text:
                    stalled.append(row['role'])
                steps=[s for s in row['replay'].splitlines() if s and not s.startswith('#')]
                blocks={i+1 for i,s in enumerate(steps) if s=='wait open:StrkrsSave:/strkrs.save'}
                fences=[int(f) for f,b in re.findall(r'^input wait: satisfied at frame (\d+); block (\d+) begins',text,re.M) if int(b) in blocks]
                captures=[int(f) for f in re.findall(r'^capture-frame: (\d+)$',text,re.M)]
                complete.append(bool(fences and captures and max(captures)>=fences[0]+3600))
            if a.f1 and invited is None:
                host=records[0];path=ROOT/host['log'];text=path.read_text(errors='replace') if path.exists() else ''
                if host['pid'] and any(int(f)>=6000 for f in re.findall(r'^capture-frame: (\d+)$',text,re.M)):
                    # The room is open and the host waits for the station: no scheduled input until frame 9000.
                    h=win_post.window(host['pid']);win_post.pin(h);time.sleep(.5)
                    win_post.key(h,win_post.VK_F1);time.sleep(1.5)
                    win_post.shot(h,out/'host/f1-hosting.png')
                    spot=out/'invite-xy.txt'  # measured from the station's joined shot while the game loads
                    win_post.click(h,*([int(v) for v in spot.read_text().split()] if spot.exists() else FORM['invite']));time.sleep(1)
                    win_post.shot(h,out/'host/f1-invited.png')
                    win_post.key(h,win_post.VK_F1)
                    invited=time.monotonic();toast_until=invited+40
                    print('invite clicked',flush=True)
            if a.f1 and time.monotonic()<toast_until:
                st=records[1]
                if st['pid']:
                    win_post.shot(win_post.window(st['pid']),out/f'station/toast-{toast_n:02}.png');toast_n+=1
            if stalled:
                (out/'replay-stall.json').write_text(json.dumps(dict(roles=sorted(set(stalled)),reason='Expected stadium load absent after final scheduled match inputs; no on-field completion claimed'),indent=2))
                break
            if complete and all(complete):break
            if time.monotonic()-printed>30:
                printed=time.monotonic()
                print(json.dumps(dict(seconds=round(printed-start,1),instances=[dict(role=r['role'],pid=r['pid'],node=r['node']) for r in records],complete=complete)),flush=True)
            if time.monotonic()-start>a.seconds+45:break
            time.sleep(.5)
        observed=call('/admin/players',None,a.service_url)
        (out/'service-players.json').write_text(json.dumps(observed,indent=2))
    finally:
        # Normal close only for this launcher's bound children. No inherited PID.
        for row,child in zip(records,children):
            if child.poll() is None and row['pid'] is not None:close_owned_window(row['pid'])
        for row,child in zip(records,children):
            try:child.wait(60)
            except subprocess.TimeoutExpired:
                child.kill();child.wait();row['forced_launcher_close']=True
            row.update(exit_code=child.returncode,contamination=contamination,valid_timing=not contamination)
        for f in files:f.close()
        (out/'manifest.json').write_text(json.dumps(records,indent=2))
    return 0 if complete and all(complete) and not contamination and all(p.returncode==0 for p in children) else 1


if __name__=='__main__':raise SystemExit(main())
