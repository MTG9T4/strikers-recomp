"""Fresh graphics-inclusive setup guarded by Job 21b's quiet-PC rule."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from job21b_launch import process_snapshot,close_owned_window
from job21_setup import ROOT,prepare,script,archive_attempt


def main():
    attempt=0
    while True:
        quiet=None
        while quiet is None or time.monotonic()-quiet<120:
            others=[p for p in process_snapshot() if p['name'].lower().startswith('strikers')]
            if others:quiet=None
            elif quiet is None:quiet=time.monotonic()
            print('Setup waiting for quiet PC: '+json.dumps(others),flush=True);time.sleep(5)
        attempt+=1
        name='job21c-final café PC' if attempt==1 else f'job21c-final-retry{attempt} café PC'
        out=prepare(ROOT/'build/phase8'/name,ROOT/'build/phase8/job21c-release-host')
        test=script(out);start=time.monotonic();owned=set();contamination=[]
        child=subprocess.Popen([str(out/'StrikersSetup.exe'),'--self-test',str(test)],cwd=out,
            env=dict(os.environ,STRIKERS_OFFLINE='1'),creationflags=subprocess.CREATE_NO_WINDOW)
        owned.add(child.pid)
        with (out/'process-observations.jsonl').open('w') as log:
            previous=None
            while child.poll() is None:
                processes=process_snapshot()
                for _ in range(8):
                    owned.update(p['pid'] for p in processes if p['parent'] in owned)
                games=[p for p in processes if p['name'].lower().startswith('strikers')]
                if games!=previous:
                    log.write(json.dumps(dict(seconds=round(time.monotonic()-start,3),processes=games))+'\n');log.flush();previous=games
                contamination=[p for p in games if p['pid'] not in owned]
                if contamination:
                    for p in games:
                        if p['pid'] in owned and p['pid']!=child.pid:close_owned_window(p['pid'])
                    close_owned_window(child.pid)
                    child.wait(60);break
                time.sleep(1)
        result=dict(exit=child.returncode,seconds=time.monotonic()-start,valid_timing=not contamination,
            contamination=contamination,status='CONTAMINATED' if contamination else 'PASS' if child.returncode==0 else 'FAILED')
        (out/'driver-result.json').write_text(json.dumps(result,indent=2));archive_attempt(out,test)
        print(json.dumps(result),flush=True)
        if not contamination:
            if child.returncode:raise RuntimeError('Fresh setup failed: '+str(out))
            (ROOT/'build/phase8/job21c-setup-result.json').write_text(json.dumps(dict(install=str(out),**result),indent=2))
            return


if __name__=='__main__':main()
