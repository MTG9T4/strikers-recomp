"""Extract a portable LLVM subset from the pinned official installer, never run it.

Developer-only: uses existing 7-Zip to prepare files to bundle for players.
Player PCs do not need 7-Zip or any system installation. Python and its wheels
are downloaded by setup, or put in installer/cache after verifying the lock.
"""
import argparse
import json
import os
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT, clean_environment, run, sha256, within


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'build/phase8/portable')
    p.add_argument('--sevenzip',type=Path,default=Path(r'C:\Program Files\7-Zip\7z.exe'))
    a=p.parse_args()
    out=within(ROOT,a.out);out.mkdir(parents=True,exist_ok=False)
    lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
    archive=ROOT/'deps/_downloads'/lock['llvm']['url'].rsplit('/',1)[1]
    if sha256(archive)!=lock['llvm']['sha256']:raise ValueError('Pinned LLVM archive hash mismatch')
    llvm=out/'toolchain/llvm'
    patterns=['bin/clang.exe','bin/lld-link.exe','bin/msvcp140.dll','bin/vcruntime140.dll',
              'bin/concrt140.dll','lib/clang/9.0.0/include/*']
    env=clean_environment(out,llvm)
    run([a.sevenzip,'x','-y','-o'+str(llvm),archive,*patterns],env,out,out/'llvm-extract.log')
    for name in ('clang.exe','lld-link.exe'):
        run([llvm/'bin'/name,'--version'],env,out,out/(name+'.version.txt'))
    files={p.relative_to(out).as_posix():sha256(p) for p in llvm.rglob('*') if p.is_file()}
    proof=dict(official_archive=lock['llvm'],files=files,bytes=sum((out/name).stat().st_size for name in files))
    (out/'toolchain-provenance.json').write_text(json.dumps(proof,indent=2))
    print(json.dumps(dict(files=len(files),bytes=proof['bytes'],out=str(out))))


if __name__=='__main__':main()
