"""Build the locally derived SDK sidecar with the same portable LLVM as main."""
import argparse
import json
from pathlib import Path
import sys
import time
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,clean_environment,run,sha256,within


def build(root,llvm,out):
    started=time.monotonic()
    root,llvm,out=map(lambda p:Path(p).resolve(),(root,llvm,out))
    out.mkdir(parents=True,exist_ok=True)
    env=clean_environment(out,llvm)
    clang=llvm/'bin/clang.exe';link=llvm/'bin/lld-link.exe'
    gen=root/'build/gen'
    sources=[gen/'sdklift_gen.c',gen/'sdkdata.c',ROOT/'tools/release/sdk_player_shim.c',ROOT/'tools/release/player_crt.c']
    # The current lift has no setjmp sites. A future generated ABI change must
    # be explicitly supported/tested, never compiled with an invented jmp_buf.
    if 'SDK_SETJMP' in sources[0].read_text():raise ValueError('This SDK lift requires a new tested setjmp bridge')
    flags=['-c','-O2','-w','-Werror=implicit-function-declaration','-ffreestanding','-fno-strict-aliasing',
           '-ffp-contract=off','-nostdinc','-isystem',ROOT/'tools/release/include',
           '-isystem',llvm/'lib/clang/9.0.0/include','-I',root/'runtime/core',
           '-I',ROOT/'tools/release','-I',gen,'-include',ROOT/'tools/release/sdk_player_min.h']
    objects=[]
    for i,source in enumerate(sources):
        obj=out/f'{i}.obj'
        run([clang,*flags,source,'-o',obj],env,out,out/f'{i}.log');objects.append(obj)
    libraries=[]
    for name in ('ucrtbase','kernelbase'):
        lib=out/(name+'.lib');libraries.append(lib)
        run([link,'/dll','/noentry','/nodefaultlib','/machine:x64',
             '/def:'+str(ROOT/f'tools/release/defs/{name}.def'),
             '/out:'+str(out/(name+'-stub.dll')),'/implib:'+str(lib)],env,out,out/(name+'.log'))
    dll=out/'strikers_sdk.dll'
    run([link,'/dll','/noentry','/nodefaultlib','/machine:x64','/dynamicbase','/nxcompat',
         '/out:'+str(dll),*objects,*libraries],env,out,out/'link.log')
    import ctypes
    ctypes.CDLL(str(dll))
    proof=dict(environment=env,dll_sha256=sha256(dll),sources={str(p):sha256(p) for p in sources},
               seconds=time.monotonic()-started,interpreter=sys.executable,python_version=sys.version)
    (out/'proof.json').write_text(json.dumps(proof,indent=2))
    print('Built locally derived SDK (never distribute): '+str(dll))
    return dll


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,default=ROOT)
    p.add_argument('--llvm',type=Path,default=ROOT/'deps/llvm')
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();within(a.root,a.output);build(a.root,a.llvm,a.output)
