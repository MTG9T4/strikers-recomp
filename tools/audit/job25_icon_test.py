"""Install a player archive into a new folder, launch it, and prove every place Windows shows the icon: none of the shipped
programs carries the game's icon (setup and the launcher carry the project's own, tools/release/app_icon.py); after setup
the shortcut and the game window show the icon setup read from the player's own files (build/game-art/icon.ico), and the
game window shows the start-up picture made from it."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zipfile

from job21_setup import ROOT, script
from job21b_launch import process_snapshot, close_owned_window
import icon_check
import win_post


def setup_window(out,results):
    child=subprocess.Popen([str(out/'StrikersSetup.exe')],cwd=out)
    end=time.monotonic()+20
    while time.monotonic()<end and not icon_check.user.FindWindowW('StrikersPlayerSetup',None):time.sleep(.2)
    time.sleep(1)
    results.append(icon_check.of_class('setup-window','StrikersPlayerSetup',out,want=False))
    icon_check.user.PostMessageW(icon_check.user.FindWindowW('StrikersPlayerSetup',None),0x10,0,0)
    child.wait(20)


def main():
    archive=Path(sys.argv[1]).resolve()
    out=ROOT/'build/phase8'/sys.argv[2]
    out.mkdir(parents=True,exist_ok=False)
    with zipfile.ZipFile(archive) as z:z.extractall(out)
    results=[icon_check.of_path('setup-file',out/'StrikersSetup.exe',out,want=False),
             icon_check.of_path('launcher-file',out/'StrikersRecomp.exe',out,want=False),
             icon_check.of_path('game-file',out/'strikers_native.exe',out,want=False)]
    setup_window(out,results)
    started=time.monotonic()
    test=script(out)
    with (out/'driver.log').open('wb') as log:
        code=subprocess.call([str(out/'StrikersSetup.exe'),'--self-test',str(test)],cwd=out,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    setup=time.monotonic()-started
    print('setup exit',code,'seconds',round(setup,1),flush=True)
    assert code==0
    icon_check.EXPECTED=out/'build/game-art/icon.ico'
    assert icon_check.EXPECTED.is_file() and (out/'build/game-art/splash.jpg').is_file(),'setup made no game art'
    results.append(icon_check.of_path('shortcut',out/'Strikers Recomp.lnk',out))
    seq=out/'launch.seq'
    seq.write_text(f'duration 200\nelf {out}/build/dump/update/elf/main.elf\nromfs {out}/build/dump/update/romfs\nclick\nwait started\nwait done\nexpect success\nexit\n',encoding='utf-8')
    env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_VOLUME='0')
    ini=out/'strikers.ini'              # a test copy stays a small window: a fullscreen one takes the desktop and keyboard
    ini.write_text(ini.read_text(encoding='utf-8')+'\nwindow = windowed\n',encoding='utf-8')   # the last value read wins
    with (out/'launch-driver.log').open('wb') as log:
        child=subprocess.Popen([str(out/'StrikersRecomp.exe'),'--self-test',str(seq)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        end=time.monotonic()+180;pid=h=None
        while time.monotonic()<end and not h:
            pids=[p['pid'] for p in process_snapshot() if p['parent']==child.pid and p['name'].lower()=='strikers_native.exe']
            if len(pids)==1:pid=pids[0];h=win_post.window(pid)
            time.sleep(.5)
        assert h,'game window did not appear'
        win_post.shot(h,out/'splash.png')
        results.append(icon_check.of_class('launcher-window','StrikersPlayerLauncher',out,want=False))   # the project's own icon
        results.append(icon_check.of_window('game-window',h,out))
        time.sleep(100)
        win_post.shot(h,out/'menu.png')
        win_post.key(h,win_post.VK_F1);time.sleep(2)
        win_post.shot(h,out/'f1-online.png')
        win_post.key(h,win_post.VK_F1);time.sleep(1)
        close_owned_window(pid)
        print('launcher exit',child.wait(90),flush=True)
    report=dict(archive=archive.name,setup_seconds=round(setup,1),setup_exit=code,launcher_exit=child.returncode,
                icons=icon_check.judge(results))
    report['icons_pass']=all(r['ok'] for r in results)
    (out/'icon-test.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2),flush=True)


if __name__=='__main__':main()
