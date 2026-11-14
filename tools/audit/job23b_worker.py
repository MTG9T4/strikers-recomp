"""Own one loopback-only Node fallback; a local stop file closes only its child."""
import argparse
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
ROOT=Path(__file__).resolve().parents[2]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--name',required=True);p.add_argument('--port',type=int,required=True)
    p.add_argument('--trace',action='store_true')
    a=p.parse_args();assert Path(a.name).name==a.name and 1024<a.port<65536
    out=ROOT/'build/run'/a.name;out.mkdir(exist_ok=False)
    with socket.socket() as check:check.bind(('127.0.0.1',a.port))
    env=dict(os.environ,GROUP_MODE='adhoc',GROUP_TEST_PORT=str(a.port))
    if a.trace:env['GROUP_TEST_TRACE']='1'
    for key in ('CLOUDFLARE_API_TOKEN','CLOUDFLARE_API_KEY','CLOUDFLARE_EMAIL','CLOUDFLARE_ACCOUNT_ID'):env.pop(key,None)
    record=dict(service_kind='Node SQLite fallback',origin=f'http://127.0.0.1:{a.port}',
                local_only=True,central='mock; no outbound fetch implementation',mode='adhoc')
    with (out/'server.log').open('wb') as log:
        child=subprocess.Popen([shutil.which('node'),'test/native-server.mjs'],cwd=ROOT/'cloudflare/group',
            env=env,stdout=log,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        record['pid']=child.pid;(out/'server.json').write_text(json.dumps(record,indent=2))
        try:
            until=time.monotonic()+30
            while 'READY' not in (out/'server.log').read_text(errors='replace'):
                if child.poll() is not None:raise RuntimeError('Fallback exited: '+(out/'server.log').read_text(errors='replace'))
                if time.monotonic()>until:raise RuntimeError('Fallback did not become ready')
                time.sleep(.1)
            print(json.dumps(record),flush=True)
            while child.poll() is None and not (out/'stop').exists():time.sleep(.5)
        finally:
            if child.poll() is None:child.terminate()
            try:child.wait(10)
            except subprocess.TimeoutExpired:child.kill();child.wait()
            record['exit']=child.returncode;record['stopped_by_controller']=(out/'stop').exists()
            (out/'server.json').write_text(json.dumps(record,indent=2))


if __name__=='__main__':main()
