"""Job 21c: continue Job 21b's serial cold/prepared launch plan.

Every attempt uses a new local cache/profile. Contaminated attempts are retained
and retried. No external game is stopped and no shared driver cache is changed.
"""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(ROOT/'tools/release'),str(ROOT/'tools/audit')]
from common import sha256
from job21b_serial import archive_generated


def baseline_install():
    out=ROOT/'build/phase8/job21c-baseline-player'
    out.mkdir(exist_ok=False)
    sources=[ROOT/'build/phase8/job21c-release-host/strikers_native.exe',
        ROOT/'build/phase8/job21c-main/strikers_game.dll',ROOT/'build/phase8/job21c-sdk/strikers_sdk.dll',
        ROOT/'build/phase8/apps/StrikersRecomp.exe',ROOT/'tools/release/strikers.ini']
    sources += [ROOT/'build/bin'/n for n in ('SDL3.dll','strikers_glslang.dll','libopus-0.dll','strikers_zt.dll')]
    sources += list((ROOT/'build/phase8/app-runtime').glob('*.dll'))
    for src in sources:shutil.copyfile(src,out/src.name)
    (out/'binary-inputs.json').write_text(json.dumps({p.name:sha256(p) for p in sources},indent=2))
    return out


def launch(label,install=None,seconds=3600,cold=False):
    attempt=0
    while True:
        attempt+=1;name=f'job21c-{label}-{attempt:02}'
        if (ROOT/'build/phase8'/name).exists():continue
        if install and cold:
            archive_generated(install,'cache',name)
            archive_generated(install,'userdata',name)
        args=[sys.executable,'-B',str(ROOT/'tools/audit/job21b_launch.py'),
            '--name',name,'--seconds',str(seconds),'--stop-frame','6000','--wait-idle','--diagnostics',
            '--elf',str(ROOT/'build/dump/update/elf/main.elf'),'--romfs',str(ROOT/'build/dump/update/romfs')]
        if install:args+=['--install',str(install)]
        print('Starting '+name,flush=True)
        with (ROOT/f'build/phase8/{name}-driver.log').open('wb') as log:
            code=subprocess.call(args,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
        path=ROOT/f'build/phase8/{name}/result.json'
        if not path.exists():raise RuntimeError('Launch helper failed; see '+str(path.parent))
        result=json.loads(path.read_text())
        if result.get('valid_timing'):
            if code or result['exit'] or result['last_frame']<6000:
                raise RuntimeError('Launch did not complete: '+name)
            return str(path.relative_to(ROOT))
        print('Discarding contaminated timing: '+name,flush=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('phase',choices=('cold','prepared'));a=p.parse_args()
    index=ROOT/'build/phase8/job21c-serial-results.json'
    results=json.loads(index.read_text()) if index.exists() else {}
    if a.phase=='cold':
        install=ROOT/'build/phase8/job21c-baseline-player'
        if not install.exists():baseline_install()
        if 'release_cold' not in results:
            results['release_cold']=launch('release-cold',install,cold=True)
            index.write_text(json.dumps(results,indent=2))
        if 'developer_cold' not in results:
            results['developer_cold']=launch('developer-cold')
            index.write_text(json.dumps(results,indent=2))
    else:
        setup=json.loads((ROOT/'build/phase8/job21c-setup-result.json').read_text())
        results['release_prepared']=launch('release-prepared',Path(setup['install']),seconds=1800)
        index.write_text(json.dumps(results,indent=2))


if __name__=='__main__':main()
