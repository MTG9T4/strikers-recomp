"""Read-only handle/phase evidence for the explicitly owned Job 21c soak."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time
from types import SimpleNamespace
from job21b_launch import strikers_processes
from run_player_soak import ROOT,metrics


def main():
    p=argparse.ArgumentParser();p.add_argument('run',type=Path);a=p.parse_args()
    out=a.run.resolve()
    if (ROOT/'build/phase8').resolve() not in out.parents:raise ValueError('Expected local evidence folder')
    while not (out/'manifest.json').exists():time.sleep(1)
    manifest=json.loads((out/'manifest.json').read_text());pid=manifest['pid']
    image=out/'bin/strikers_native.exe';census=ROOT/'build/phase8/apps/handle-census.exe'
    offset=0;pending='';next_sample=0;last_wait=None;start=time.monotonic()
    with (out/'handle-phase-samples.jsonl').open('x') as dest:
        while any(p['pid']==pid for p in strikers_processes()):
            path=out/'game.log'
            if manifest.get('launcher_pid'):path=out/'bin/userdata/last-game.log'
            with path.open('r',errors='replace') as source:
                source.seek(offset);pending+=source.read();offset=source.tell()
            lines=pending.split('\n');pending=lines.pop();captures=[]
            for line in lines:
                m=re.search(r'input wait: satisfied at frame (\d+); block (\d+) begins',line)
                if m:last_wait=dict(frame=int(m[1]),block=int(m[2]))
                m=re.match(r'capture-frame: (\d+)',line)
                if m:captures.append(int(m[1]))
            now=time.monotonic()
            if now>=next_sample or captures:
                row=dict(observer_seconds=round(now-start,3),captures=captures,last_wait=last_wait)
                try:row['resources']=metrics(SimpleNamespace(pid=pid))
                except OSError:break
                result=subprocess.run([str(census),str(pid),str(image)],capture_output=True,text=True,
                    creationflags=subprocess.CREATE_NO_WINDOW)
                row['census_exit']=result.returncode
                if not result.returncode:row['census']=json.loads(result.stdout)
                else:row['error']=result.stdout+result.stderr
                dest.write(json.dumps(row)+'\n');dest.flush();next_sample=now+60
            time.sleep(1)


if __name__=='__main__':main()
