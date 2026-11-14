"""Run the real launcher window's script loop on failure and native pad checks.

No computer-use API, synthetic backend state, or simulated success is used.
The pad host exits before starting a game; this is not Quick Battle evidence.
"""
from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import argparse
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools/release'))
from common import sha256,clean_environment

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--name',default='job21-launcher-gui')
    parser.add_argument('--host',type=Path,default=ROOT/'build/phase8/release-host/strikers_native.exe')
    parser.add_argument('--dll',type=Path,default=ROOT/'build/phase8/portable-full/strikers_game.dll')
    parser.add_argument('--sdk',type=Path,default=ROOT/'build/phase8/player-sdk/strikers_sdk.dll')
    args=parser.parse_args()
    from common import within
    output=within(ROOT/'build/phase8',ROOT/'build/phase8'/args.name);output.mkdir(exist_ok=False)
    results=[]
    for name in ('missing-host','native-pad'):
        out=output/name;out.mkdir()
        launcher=out/'StrikersRecomp.exe';shutil.copyfile(ROOT/'build/phase8/apps'/launcher.name,launcher)
        env=clean_environment(out,ROOT/'build/phase8/portable2/toolchain/llvm')
        env['PATH']=str(Path(env['SystemRoot'])/'System32')
        if name=='native-pad':
            for filename in ('SDL3.dll','strikers_glslang.dll','strikers_zt.dll','libopus-0.dll'):
                shutil.copyfile(ROOT/'build/bin'/filename,out/filename)
            shutil.copyfile(args.host,out/'strikers_native.exe')
            shutil.copyfile(args.dll,out/'strikers_game.dll')
            shutil.copyfile(args.sdk,out/'strikers_sdk.dll')
            for p in (ROOT/'build/phase8/app-runtime').glob('*.dll'):shutil.copyfile(p,out/p.name)
            env['STRIKERS_PAD_SELFTEST']='1'
        script=out/'launcher-test.seq'
        script.write_text('snapshot launcher-start\nclick\nwait done\nsnapshot launcher-final\nexpect '+
            ('success' if name=='native-pad' else 'failure')+'\nexit\n')
        result=subprocess.run([str(launcher),'--self-test',str(script)],cwd=out,env=env,timeout=60)
        if result.returncode:raise AssertionError('Real launcher GUI test failed: '+name)
        if name=='native-pad':
            if b'PAD SELFTEST PASSED' not in (out/'userdata/last-game.log').read_bytes():
                raise AssertionError('Real native host pad check did not complete')
        results.append(dict(case=name,status='PASS',launcher_sha256=sha256(launcher),
            scope='native launcher window/process test; no game started'))
    (output/'result.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))

if __name__=='__main__':main()
