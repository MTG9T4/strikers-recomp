"""Run the Job 21b graphics comparisons serially, rejecting overlapping probes."""
import json
from pathlib import Path
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import within


def archive_generated(install,name,label):
    src=within(install,install/name)
    if not src.exists():return
    if src.is_symlink() or src.is_junction():raise ValueError('Unexpected generated junction')
    dest=within(install,install/(name+'-'+label))
    if dest.exists():raise ValueError('Evidence archive already exists')
    src.rename(dest)


def launch(label,install=None,seconds=3600,cold=False):
    attempt=0
    while True:
        attempt+=1;name=f'job21b-{label}-{attempt:02}'
        if install and cold:
            archive_generated(install,'cache',name)
            archive_generated(install,'userdata',name)
        args=[sys.executable,'-B',str(ROOT/'tools/audit/job21b_launch.py'),
            '--name',name,'--seconds',str(seconds),'--stop-frame','6000','--wait-idle']
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
        print('Discarding overlapping timing; preserving all evidence: '+name,flush=True)


def main():
    initial=ROOT/'build/phase8/job21b-e2e-player café PC'
    final=ROOT/'build/phase8/job21b-final café PC'
    results={}
    results['release_cold']=launch('release-cold',initial,cold=True)
    (ROOT/'build/phase8/job21b-serial-results.json').write_text(json.dumps(results,indent=2))
    results['developer_cold']=launch('developer-cold')
    (ROOT/'build/phase8/job21b-serial-results.json').write_text(json.dumps(results,indent=2))
    print('Starting final fresh setup',flush=True)
    with (ROOT/'build/phase8/job21b-final-setup-driver.log').open('wb') as log:
        code=subprocess.call([sys.executable,'-B',str(ROOT/'tools/audit/job21_setup.py'),
            '--name',final.name,'--retry'],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    if code:raise RuntimeError('Final setup failed; see its installer/setup.log')
    results['release_prepared']=launch('release-prepared',final,seconds=1800)
    (ROOT/'build/phase8/job21b-serial-results.json').write_text(json.dumps(results,indent=2))


if __name__=='__main__':main()
