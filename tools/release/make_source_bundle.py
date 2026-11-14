"""Offline corresponding-source preparation for exact local release artifacts.

Uses git archive of an immutable commit plus an explicit working-tree overlay.
Never fabricates completeness when upstream source archives are unavailable.
General-purpose Python/LLVM tools and Microsoft system runtimes are indexed
separately from the GPL program's corresponding-source closure.
"""
import argparse
import ast
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,sha256,within
from make_release import audit_host,RUNTIME_BINARIES,safe_entry

GIT=['git','-c','safe.directory='+ROOT.as_posix()]

def git(*args):
    return subprocess.check_output([*GIT,*args],cwd=ROOT)

def neutral_paths(data,root=ROOT):
    # The generated ZeroTier files name this checkout. A player's source copy must not show the packager's folder.
    for form in (root.as_posix(),str(root),str(root).replace('\\','\\\\')):
        data=data.replace(form.encode('utf-8'),b'@STRIKERS_SOURCE_ROOT@')
    return data


def overlay_paths(commit,new_sources=(),root=ROOT):
    command=['git','-c','safe.directory='+root.as_posix()]
    modified=subprocess.check_output([*command,'diff',commit,'--name-only','-z'],cwd=root).decode('utf-8').split('\0')
    for name in new_sources:
        safe_entry(name)
        if not name.startswith(('installer/','tools/release/','tools/audit/','runtime/')) or not within(root,root/name).is_file():
            raise ValueError('Invalid explicitly included new source: '+name)
    # Never discover untracked outputs implicitly, including graphics results.
    return sorted((set(modified)|set(new_sources))-{''})


def make(out,host,provenance,commit='HEAD',new_sources=(),exclude_service=False):
    out=within(ROOT,out);out.mkdir(parents=True,exist_ok=False)
    audit=audit_host(host,provenance)
    if not audit['game_free']:raise ValueError('Host source provenance mismatch: '+str(audit['reasons']))
    release_commit=git('rev-parse',commit+'^{commit}').decode().strip()
    archive=out/'project-source.tar'
    # Historical audit reports contain translated guest-code examples. They
    # are not Corresponding Source for this program and cannot enter delivery.
    scope=[name for name in git('ls-tree','--name-only',release_commit).decode().splitlines() if name!='docs']
    if exclude_service:
        # Independently deployed service is not a build input for the player
        # binaries. Do not open its production configuration during player work.
        scope=[name for name in scope if name!='cloudflare']
    docs=set(git('ls-tree','-r','--name-only',release_commit,'--','docs').decode().splitlines())
    scope+=[name for name in ('docs/PLAYING.md','docs/phase8-installer.md') if name in docs]   # the public copy has no installer record
    subprocess.run([*GIT,'archive','--format=tar','--output='+str(archive),release_commit,'--',*scope],cwd=ROOT,check=True)
    files={'project-source.tar':sha256(archive)}
    overlay={}
    # Include tracked modifications and explicitly named new release sources.
    # Never read ignored cache/build/dump/key data into a source overlay.
    for name in overlay_paths(release_commit,new_sources):
        if exclude_service and name.startswith('cloudflare/'):continue
        if name.startswith('docs/') and name not in ('docs/PLAYING.md','docs/phase8-installer.md'):continue
        safe_entry(name)
        src=within(ROOT,ROOT/name)
        if not src.exists():overlay[name]=None;continue
        if not src.is_file():raise ValueError('Unexpected source overlay path')
        dst=out/'working-tree-overlay'/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dst)
        overlay[name]=sha256(dst);files[dst.relative_to(out).as_posix()]=sha256(dst)
    committed=set(git('ls-tree','-r','--name-only',release_commit).decode().splitlines())
    build_proof=json.loads(Path(provenance).read_text())
    required={r['source'] for r in build_proof['sources'] if r['source'].startswith(('runtime/','tools/release/'))}|{'tools/release/host.manifest'}
    absent=required-committed-set(overlay)
    if absent:raise ValueError('Explicit --new-source required for build inputs: '+', '.join(sorted(absent)))
    (out/'overlay-manifest.json').write_text(json.dumps(overlay,indent=2))
    files['overlay-manifest.json']=sha256(out/'overlay-manifest.json')
    tree=ast.parse((ROOT/'scripts/fetch_deps.py').read_text())
    pins={}
    for node in tree.body:
        if isinstance(node,ast.Assign) and any(isinstance(t,ast.Name) and t.id in ('TOOLS','LIBZT') for t in node.targets):
            pins[node.targets[0].id]=ast.literal_eval(node.value)
    dependencies=[];missing=[];archives={}
    def add_archive(name,url,digest,cache,source=True):
        record=dict(name=name,url=url,sha256=digest,source=source,available=cache.is_file())
        if cache.is_file():
            if sha256(cache)!=digest:raise ValueError('Dependency cache hash mismatch: '+name)
            dst=out/'dependencies'/cache.name;dst.parent.mkdir(exist_ok=True)
            removed=[]
            if name=='libzt-ext_ZeroTierOne':
                # This unrelated upstream Jenkins lint helper contains a hard-
                # coded credential. It is not referenced by the program build.
                # Keep the pinned input untouched and omit only this exact CI
                # utility from delivery; all program/build bytes remain intact.
                with zipfile.ZipFile(cache) as original:
                    omit=[i for i in original.infolist() if i.filename.endswith('/dockerbuild/pipelint.sh')]
                    if len(omit)!=1:raise ValueError('Upstream CI-helper exclusion anchor changed')
                    import hashlib
                    removed=[dict(path=omit[0].filename,sha256=hashlib.sha256(original.read(omit[0])).hexdigest(),
                        reason='Unrelated credential-bearing Jenkins CI lint helper; not used by the program build')]
                    with zipfile.ZipFile(dst,'x') as delivered:
                        for item in original.infolist():
                            if item.filename!=omit[0].filename:delivered.writestr(item,original.read(item))
            else:shutil.copyfile(cache,dst)
            delivered_digest=sha256(dst)
            relative=dst.relative_to(out).as_posix();files[relative]=delivered_digest
            record.update(file=relative,delivered_sha256=delivered_digest)
            archives[relative]=dict(sha256=delivered_digest,source=source)
            if removed:
                record['excluded_non_build_files']=removed
                archives[relative].update(upstream_sha256=digest,excluded_non_build_files=removed)
        elif source:missing.append(name+': source archive is not cached')
        dependencies.append(record)
    for name in ('imgui','opus','vulkan-headers'):
        url,digest,*_=pins['TOOLS'][name]
        add_archive(name,url,digest,ROOT/'deps/_downloads'/url.rsplit('/',1)[-1])
    cached=['libzt-root-','libzt-ext_ZeroTierOne-','libzt-ext_lwip-','libzt-ext_lwip-contrib-']
    for prefix,(url,digest,_) in zip(cached,pins['LIBZT']):
        cache=ROOT/'deps/_downloads'/(prefix+url.rsplit('/',1)[-1])
        add_archive(prefix.rstrip('-'),url,digest,cache)
    # These pinned binary archives are not corresponding source substitutes.
    for name in ('glslang','sdl3'):
        url,digest,*_=pins['TOOLS'][name]
        dependencies.append(dict(name=name,binary_url=url,binary_sha256=digest,
            source_url={'glslang':'https://github.com/KhronosGroup/glslang/tree/16.6.0',
                        'sdl3':'https://github.com/libsdl-org/SDL/tree/release-3.4.18'}[name],
            source_sha256=None,available=False))
    source_lock=json.loads((ROOT/'tools/release/dependency_sources.lock.json').read_text())
    for entry in source_lock['sources']:
        cache=within(ROOT/'deps/_downloads',ROOT/'deps/_downloads'/entry['cache'])
        digest=entry['sha256']
        if not digest or len(digest)!=64:
            dependencies.append(dict(entry,available=False))
            missing.append(entry['name']+': reviewed source SHA-256 is missing; cached source present='+str(cache.is_file()))
        else:add_archive(entry['name'],entry['url'],digest,cache)
    # Preserve exact ZeroTier build adaptations and CMake recipe.
    for name in ('CMakeLists.txt','adaptations.json','NodeService.cpp','VirtualTap.cpp','lwipopts.h','Network.cpp'):
        src=ROOT/'build/libzt'/name
        dst=out/'libzt-build'/name;dst.parent.mkdir(exist_ok=True);dst.write_bytes(neutral_paths(src.read_bytes()))
        files[dst.relative_to(out).as_posix()]=sha256(dst)
    lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
    tools=[]
    for entry in [lock['python'],lock['llvm'],*lock['wheels']]:
        tools.append(dict(entry,role='General-purpose unmodified build tool; not a substitute for program source'))
    binary_hashes={'strikers_native.exe':sha256(host)}
    for name in RUNTIME_BINARIES[1:]:binary_hashes[name]=sha256(ROOT/'build/bin'/name)
    for name in ('StrikersRecomp.exe','StrikersSetup.exe'):binary_hashes[name]=sha256(ROOT/'build/phase8/apps'/name)
    for path in (ROOT/'build/phase8/app-runtime').glob('*.dll'):binary_hashes[path.name]=sha256(path)
    toolchain=ROOT/'build/phase8/portable2/toolchain/llvm'
    for path in toolchain.rglob('*'):
        if path.is_file():binary_hashes['toolchain/llvm/'+path.relative_to(toolchain).as_posix()]=sha256(path)
    for entry in [lock['python'],*lock['wheels']]:
        name=entry['url'].rsplit('/',1)[-1];binary_hashes['installer/cache/'+name]=entry['sha256']
    instructions='''Exact source snapshot and rebuild instructions

Extract project-source.tar into an empty directory. Apply working-tree-overlay
over it; remove paths whose overlay-manifest value is null. The manifest hashes
describe the resulting uncommitted changes; do not substitute a later checkout.
Unpack each dependency archive into the deps path described by scripts/fetch_deps.py.
No package manager or automatic dependency download was run for this bundle.
The ZeroTierOne delivery archive omits one unrelated credential-bearing Jenkins
lint helper. Its original upstream checksum and exact omission are recorded in
source-manifest.json. All program and build files retain their original bytes.

tools/lifter/build_exe.py documents/builds the Opus and glslang DLLs.
tools/audit/zt_sdk_build.py and libzt-build retain the exact local ZeroTier
adaptations, hashes and CMake build recipe. Replace @STRIKERS_SOURCE_ROOT@ in
libzt-build with your source folder, using forward slashes. tools/release/build_apps.py builds
the native setup and launcher; tools/release/build_release_host.py builds the
separate game-free host. These developer builds use existing licensed MSVC and
Windows/Vulkan headers. Player setup builds only the two local game-derived
DLLs using pinned portable LLVM, as documented in docs/phase8-installer.md.

This source preparation is INCOMPLETE until source-manifest.json has complete
true and no missing items. A homepage URL or binary archive does not close a
missing-source gate. Never label this directory complete solely because git
archive succeeded. No game, keys or translated game/SDK code is included.
'''
    (out/'BUILD-SOURCE.txt').write_text(instructions)
    files['BUILD-SOURCE.txt']=sha256(out/'BUILD-SOURCE.txt')
    report=dict(schema=2,release_commit=release_commit,archive_path_scope=scope,
        excluded='Historical audit docs with translated guest-code examples; not program Corresponding Source'+
                 ('; independently deployed cloudflare service, not a player-binary build input' if exclude_service else ''),host_sha256=audit['sha256'],
        binary_sha256=binary_hashes,complete=not missing,missing=missing,
        files=files,archives=archives,dependencies=dependencies,build_tools=tools,
        overlay=overlay,source_delivery='Accompanying source under GPL section 6(a); written offer is additional, never a replacement')
    (out/'source-manifest.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(dict(complete=report['complete'],missing=missing,release_commit=release_commit,files=len(files)),indent=2))
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'build/phase8/corresponding-source')
    p.add_argument('--host',type=Path,default=ROOT/'build/phase8/release-host/strikers_native.exe')
    p.add_argument('--host-provenance',type=Path,default=ROOT/'build/phase8/release-host/provenance.json')
    p.add_argument('--new-source',action='append',default=[],help='explicitly reviewed new source path; untracked files are never discovered')
    p.add_argument('--exclude-service',action='store_true',help='omit the independently deployed service without opening its configuration')
    p.add_argument('--commit',default='HEAD');a=p.parse_args()
    result=make(a.out,a.host,a.host_provenance,a.commit,a.new_source,a.exclude_service)
    raise SystemExit(0 if result['complete'] else 1)
