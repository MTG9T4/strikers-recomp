"""Assemble an exact player-file allowlist, without publishing or making a ZIP.

Requires completed dependency notices and source delivery. Does not copy any
generated game/SDK DLL, lifted file, dump, key, save, or identity.
"""
import json
from pathlib import Path
import shutil
import sys
import argparse
import subprocess
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,sha256,within
from make_release import audit_host,safe_entry,RUNTIME_BINARIES
from collect_notices import collect


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--candidate',action='store_true',help='prepare a clearly marked local review candidate; never a player release')
    parser.add_argument('--out',type=Path,default=ROOT/'build/phase8/player-stage')
    parser.add_argument('--notices',type=Path,default=ROOT/'build/phase8/notices')
    parser.add_argument('--source',type=Path,default=ROOT/'build/phase8/corresponding-source')
    parser.add_argument('--host-dir',type=Path,default=ROOT/'build/phase8/release-host')
    parser.add_argument('--version',help='player release version, such as 1.0.4: the in-game updater compares it')
    args=parser.parse_args()
    host=args.host_dir/'strikers_native.exe'
    result=audit_host(host,args.host_dir/'provenance.json')
    if not result['game_free']:raise ValueError('Host provenance failed: '+str(result['reasons']))
    notices=within(ROOT,args.notices)
    if collect(notices)['status']!='PASS' and not args.candidate:raise ValueError('Dependency notices are incomplete')
    # This evidence must describe complete corresponding sources for the exact
    # prebuilt DLLs, not merely a generic GPL promise or upstream homepage URL.
    source=within(ROOT,args.source)
    index=source/'source-manifest.json'
    if not index.exists():raise ValueError('Complete corresponding source bundle has not been prepared')
    proof=json.loads(index.read_text())
    if proof.get('host_sha256')!=result['sha256'] or (not proof.get('complete') and not args.candidate):
        raise ValueError('Corresponding source does not match this host')
    stage=within(ROOT,args.out);stage.mkdir(parents=True,exist_ok=False)
    files={}
    def add(src,name):
        safe_entry(name);dest=within(stage,stage/name)
        if name in files:raise ValueError('Duplicate stage file: '+name)
        dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest);files[name]=sha256(dest)
    add(host,'strikers_native.exe')
    if json.loads((ROOT/'build/libzt/adaptations.json').read_text())['group_filter_sha256']!=sha256(ROOT/'runtime/services/zt_group_filter.h'):
        raise ValueError('strikers_zt.dll predates the group filter; rebuild it')
    if sha256(ROOT/'build/bin/strikers_zt.dll')!=sha256(ROOT/'build/libzt/msvc/Release/strikers_zt.dll'):
        raise ValueError('strikers_zt.dll differs from the checked SDK build')
    for name in RUNTIME_BINARIES[1:]:add(ROOT/'build/bin'/name,name)
    for name in ('StrikersSetup.exe','StrikersRecomp.exe'):add(ROOT/'build/phase8/apps'/name,name)
    add(ROOT/'tools/release/README-PLAYERS.txt','README-PLAYERS.txt')
    add(ROOT/'tools/release/strikers.ini','strikers.ini')
    add(ROOT/'docs/PLAYING.md','docs/PLAYING.md')
    add(ROOT/'LICENSE','LICENSE')
    # Source tools are shipped as source, not frozen with unlicensed game data.
    for name in tracked_sources():
        if name not in files:add(ROOT/name,name)
    for name in ('tools/target.py','scripts/identify.py'):add(ROOT/name,name)
    for path in (ROOT/'build/phase8/portable2/toolchain/llvm').rglob('*'):
        if path.is_file():add(path,'toolchain/llvm/'+path.relative_to(ROOT/'build/phase8/portable2/toolchain/llvm').as_posix())
    for name in ('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll'):
        if not (ROOT/'build/phase8/app-runtime'/name).is_file():raise ValueError('Missing app-local runtime: '+name)
    for path in (ROOT/'build/phase8/app-runtime').glob('*.dll'):add(path,path.name)
    for path in notices.rglob('*'):
        if path.is_file():add(path,'licenses/'+path.relative_to(notices).as_posix())
    # Ship the exact hash-verified cache so setup need not access the network.
    lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
    for entry in [lock['python'],*lock['wheels']]:
        name=entry['url'].rsplit('/',1)[-1];path=ROOT/'installer/cache'/name
        if sha256(path)!=entry['sha256']:raise ValueError('Tool cache hash mismatch: '+name)
        add(path,'installer/cache/'+name)
    for name,digest in proof['files'].items():
        path=within(source,source/name)
        if sha256(path)!=digest:raise ValueError('Corresponding-source hash mismatch')
        add(path,'corresponding-source/'+name)
    add(index,'corresponding-source/source-manifest.json')
    manifest=dict(schema=2,game_free_host=True,candidate_only=args.candidate,files=files,host_sha256=result['sha256'])
    if args.version:manifest['version']=args.version
    if args.candidate:
        note=stage/'RELEASE-CANDIDATE.txt'
        note.write_text('LOCAL REVIEW CANDIDATE — NOT FOR DISTRIBUTION\nAcceptance, source and notice gates are recorded separately. This name never denotes a player release.\n',encoding='utf-8')
        files[note.name]=sha256(note)
    (stage/'release-manifest.json').write_text(json.dumps(manifest,indent=2))
    print(f'Staged {len(files)} game-free files at {stage}')


def tracked_sources(root=ROOT):
    tracked=subprocess.check_output(['git','-c','safe.directory='+root.as_posix(),'ls-files','-z','--',
        'installer','tools/release','tools/extract','tools/lifter','runtime'],cwd=root).decode('utf-8').split('\0')
    return [name for name in sorted(filter(None,tracked)) if Path(name).suffix.lower() in
        ('.py','.c','.cpp','.h','.inc','.json','.txt','.def','.cs','.manifest','.rtf','.rst')]


if __name__=='__main__':main()
