"""Unpack the player ZIP into a new folder, run setup from there on the real files, then launch and open F1."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import zipfile

from job21_setup import ROOT, script
from job21b_launch import process_snapshot, close_owned_window
import win_post


def main():
    archive=Path(sys.argv[1]).resolve()
    out=ROOT/'build/phase8'/sys.argv[2]
    out.mkdir(parents=True,exist_ok=False)
    with zipfile.ZipFile(archive) as z:z.extractall(out)
    started=time.monotonic()
    test=script(out)
    with (out/'driver.log').open('wb') as log:
        code=subprocess.call([str(out/'StrikersSetup.exe'),'--self-test',str(test)],cwd=out,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    setup=time.monotonic()-started
    print('setup exit',code,'seconds',round(setup,1),flush=True)
    assert code==0
    seq=out/'launch.seq'
    seq.write_text(f'duration 200\nelf {out}/build/dump/update/elf/main.elf\nromfs {out}/build/dump/update/romfs\nclick\nwait started\nwait done\nexpect success\nexit\n',encoding='utf-8')
    env={k:v for k,v in os.environ.items() if not k.startswith('STRIKERS_')}
    env.update(STRIKERS_SHOW='1',STRIKERS_VOLUME='0')
    with (out/'launch-driver.log').open('wb') as log:
        child=subprocess.Popen([str(out/'StrikersRecomp.exe'),'--self-test',str(seq)],cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        end=time.monotonic()+180;pid=h=None
        while time.monotonic()<end and not h:
            pids=[p['pid'] for p in process_snapshot() if p['parent']==child.pid and p['name'].lower()=='strikers_native.exe']
            if len(pids)==1:pid=pids[0];h=win_post.window(pid)
            time.sleep(.5)
        assert h,'game window did not appear'
        time.sleep(100)
        win_post.shot(h,out/'menu.png')
        win_post.key(h,win_post.VK_F1);time.sleep(2)
        win_post.shot(h,out/'f1-online.png')
        win_post.key(h,win_post.VK_F1);time.sleep(1)
        close_owned_window(pid)
        print('launcher exit',child.wait(90),flush=True)
    (out/'zip-test.json').write_text(json.dumps(dict(archive=archive.name,setup_seconds=round(setup,1),setup_exit=code,launcher_exit=child.returncode),indent=2))


if __name__=='__main__':main()
