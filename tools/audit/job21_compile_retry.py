"""Real embedded-Python compiler interruption/retry; NOT setup E2E acceptance.

Uses the existing locally generated lift, never a foreign object or DLL. This
independent recovery check remains useful while the selected keys are missing.
"""
import json
from pathlib import Path
import subprocess
import sys
import time
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import clean_environment
from windows_process import run_compiler

def main():
    python=ROOT/'build/phase8/e2e-job21-wrong-keys-final/toolchain/python/python.exe'
    out=ROOT/'build/phase8/job21-embedded-retry-main';out.mkdir(exist_ok=False)
    llvm=ROOT/'build/phase8/portable2/toolchain/llvm'
    env=clean_environment(out/'orchestrator',llvm)
    command=[python,'-I',ROOT/'tools/release/build_player.py','--root',ROOT,'--llvm',llvm,'--output',out/'main','--jobs','4']
    started=time.monotonic()
    code,stdout,stderr=run_compiler(command,env,ROOT,8)
    (out/'interrupted.log').write_text(stdout+stderr)
    if code is not None:raise AssertionError('Expected a real interrupted compiler parent')
    receipts=list((out/'main/obj').glob('*.receipt.json'))
    interrupted=time.monotonic()-started
    start_retry=time.monotonic()
    with (out/'retry.log').open('wb') as log:
        result=subprocess.run([str(x) for x in [*command,'--link-existing']],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:raise RuntimeError('Actual embedded retry failed; see retry.log')
    proof=json.loads((out/'main/proof.json').read_text())
    reused=sum(r['optimization']=='verified prior successful compile' for r in proof['sources'])
    if not reused:raise AssertionError('No atomically completed object was reused')
    report=dict(status='PASS',scope='standalone compile recovery from existing lift; not setup E2E',
        embedded_python=subprocess.check_output([str(python),'--version'],env=env,text=True).strip(),
        interruption_seconds=interrupted,successful_receipts_before_interruption=len(receipts),
        reused_objects=reused,retry_seconds=time.monotonic()-start_retry,game_sha256=proof['dll_sha256'])
    (out/'result.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))

if __name__=='__main__':main()
