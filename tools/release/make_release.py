"""Fail-closed Strikers release packager. Never accepts a developer host by name.

--check-host records why the current host is ineligible. --review-tools makes
an explicitly non-player review archive of this job's tools only. Neither is
a workaround for the ordinary release gate.
"""
import argparse
import ast
import json
from pathlib import Path, PurePosixPath
import re
import sys
import zipfile
import io
import tarfile
import struct
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT, sha256, within
from pe import PE

BLOCKED_PARTS={'romfs','exefs','dump','userdata','lift','obj','.git','.venv','__pycache__'}
BLOCKED_SUFFIX={'.xci','.nsp','.nca','.nsz','.xcz','.keys','.elf','.nso','.npdm','.nacp','.obj','.pdb','.ilk','.dmp','.save'}
RUNTIME_BINARIES=('strikers_native.exe','strikers_glslang.dll','libopus-0.dll','strikers_zt.dll','SDL3.dll')
VENDORED_HOST_SOURCES={f'deps/imgui/{name}.cpp' for name in ('imgui','imgui_draw','imgui_tables','imgui_widgets')}


# Compressed containers this audit cannot open. Nothing shipped uses them; one must not pass as an opaque blob.
OPAQUE_ARCHIVES=(b'\xfd7zXZ\x00',b"7z\xbc\xaf'\x1c",b'\x28\xb5\x2f\xfd',b'\x04\x22\x4d\x18',b'MSCF',b'Rar!\x1a\x07',
                 *(b'BZh%d1AY&SY'%n for n in range(1,10)))
IMAGE_SAMPLES=32


def packager_account(data):
    # Audit 09: every file, text too. Upstream sources hold paths such as C:\Users\bob, so text is matched on this name only.
    account=Path.home().name
    return len(account)>=6 and any(re.search(re.escape(account.encode(e)),data,re.I) for e in ('utf-8','utf-16le'))


def private_binary_path(data):
    return bool(re.search(rb'[a-z]:[\\/]+users[\\/]',data,re.I) or
                re.search(rb'[a-z]\x00:\x00(?:[\\/]\x00)+u\x00s\x00e\x00r\x00s\x00[\\/]\x00',data,re.I)) or packager_account(data)


def safe_entry(name):
    p=PurePosixPath(name)
    if p.is_absolute() or '..' in p.parts or '\\' in name or ':' in name:
        raise ValueError('Unsafe ZIP entry: '+name)
    lower=name.lower()
    if set(s.lower() for s in p.parts)&BLOCKED_PARTS or p.suffix.lower() in BLOCKED_SUFFIX:
        raise ValueError('Game/private/generated file forbidden: '+name)
    if p.name.lower() in ('strikers_game.dll','strikers_sdk.dll','request.json') or re.fullmatch(r'c_\d+\.c',p.name):
        raise ValueError('Game-derived file forbidden: '+name)
    if p.name in ('sdkdata.c','sdklift_gen.c','image.h','funcs.h','table.c','imports.c','identity.secret','group-credential.dat') or (p.name=='eh_resume.c' and name!='runtime/libc/eh_resume.c'):
        raise ValueError('Generated or private file forbidden: '+name)


def guest_fingerprints(root=ROOT):
    # Detects the actual SDK byte arrays compiled into today's developer host,
    # rather than assuming that a harmless .exe filename proves provenance.
    path=root/'build/gen/sdkdata.c'
    if not path.exists():raise ValueError('build/gen/sdkdata.c is required to prove the release is game-free')
    result=[]
    for values in re.findall(r'static const unsigned char d\d+\[\] = \{([\d,]+)\}',path.read_text()):
        data=bytes(int(n) for n in values.split(','))
        if len(data)>=256:
            result.append(data[:256])
            result.append(data[len(data)//2:len(data)//2+256])
    if not result:raise ValueError('No SDK fingerprints found')
    # Audit 09: only one sdkdata.c array reaches 256 bytes, so that file alone gives two samples. Sample the images
    # themselves too. The samples stay in memory; nothing is written anywhere.
    for name in ('main.elf','sdk.elf','subsdk0.elf'):
        image=root/'build/dump/update/elf'/name
        if not image.is_file():raise ValueError(name+' is required to prove the release is game-free')
        data=image.read_bytes();step=max(256,len(data)//IMAGE_SAMPLES)
        result+=[s for s in (data[at:at+256] for at in range(step,len(data)-256,step)) if len(set(s))>=64]
    return result


def audit_host(host,provenance=None):
    host=Path(host)
    data=host.read_bytes()
    reasons=[]
    if private_binary_path(data):reasons.append('Local user path in release host')
    hits=sum(x in data for x in guest_fingerprints())
    if hits:reasons.append(f'Host embeds {hits} fingerprint(s) of game SDK data from sdkdata.c')
    if provenance is None or not Path(provenance).exists():
        reasons.append('Missing source-by-source game-free host build provenance')
    else:
        proof=json.loads(Path(provenance).read_text())
        if proof.get('local_group_test') or proof.get('group_self_test'):reasons.append('Numeric-loopback/self-test host is not eligible for player delivery')
        if proof.get('host_sha256')!=sha256(host):reasons.append('Host hash differs from build provenance')
        generated=Path(provenance).parent/'strikers_game.lib'
        if not generated.is_file() or proof.get('main_import_lib')!=sha256(generated):
            reasons.append('Host import library is not the generated game-shim metadata')
        definitions=Path(provenance).parent/'game-imports.def'
        if not definitions.is_file() or definitions.read_text()!='LIBRARY strikers_game.dll\nEXPORTS\n game_bind\n game_eh_bind\n':
            reasons.append('Missing generated game-shim export definitions')
        if proof.get('layout_sha256') and proof['layout_sha256']!=sha256(ROOT/'tools/release/host_layout.json'):
            reasons.append('Factual host-layout provenance mismatch')
        records=proof.get('sources',[])
        if not records:reasons.append('No host source/object records')
        for record in records:
            source=record.get('source','').replace('\\','/')
            if (not source.startswith(('runtime/','tools/release/')) and source not in VENDORED_HOST_SOURCES) or '/gen/' in source or 'sdklift_gen' in source or 'sdkdata' in source:
                reasons.append('Host source is generated/unapproved: '+source)
                continue
            path=within(ROOT,ROOT/source)
            if not path.is_file() or record.get('sha256')!=sha256(path):
                reasons.append('Source provenance mismatch: '+source)
    imports=PE(host).imports()
    return dict(host=str(host),sha256=sha256(host),game_free=not reasons,reasons=reasons,imports=imports)


def verify_zip(path,expected,fingerprints=None):
    fingerprints=guest_fingerprints() if fingerprints is None else fingerprints
    import hashlib
    with zipfile.ZipFile(path) as z:
        approved=set()
        source_index='corresponding-source/source-manifest.json'
        if source_index in z.namelist():
            source=json.loads(z.read(source_index))
            approved.update({r['sha256'] for r in source.get('archives',{}).values()})
            if source.get('files',{}).get('project-source.tar'):approved.add(source['files']['project-source.tar'])
        else:approved=set()
        expanded=0
        nested_files=0
        def inspect(name,data,depth=0):
            nonlocal expanded,nested_files
            safe_entry(name)
            if name=='runtime/libc/eh_resume.c' and hashlib.sha256(data).hexdigest()!=sha256(ROOT/name):
                raise ValueError('Unverified replacement of own exception-runtime source')
            expanded+=len(data)
            if depth>4 or expanded>2*1024**3:raise ValueError('Archive nesting/expanded size exceeds audit limit')
            if any(sample in data for sample in fingerprints):raise ValueError('Embedded game SDK bytes: '+name)
            if data.startswith((b'\x7fELF',b'NSO0',b'PFS0',b'HFS0')):
                raise ValueError('Game package/image content: '+name)
            if packager_account(data):raise ValueError('Local user name in shipped file: '+name)
            if data.startswith(OPAQUE_ARCHIVES):raise ValueError('Uninspectable compressed content: '+name)
            if data.startswith(b'MZ'):
                if private_binary_path(data):
                    raise ValueError('Local user path in shipped binary: '+name)
                try:exports=PE(data,allow_pe32=True).exports()
                except (ValueError,struct.error,IndexError) as error:raise ValueError('Uninspectable PE source entry: '+name+': '+str(error)) from None
                if set(exports)&{'game_bind','game_eh_bind','player_sdk_bind'}:raise ValueError('Renamed game DLL: '+name)
            if re.search(rb'(?m)^(?:static )?void (?:f|u)_7[15][0-9a-f]{8}\(',data):
                raise ValueError('Lifted guest C content: '+name)
            if re.search(rb'(?im)^\s*(?:[a-z0-9_]*(?:key|kek|source)[a-z0-9_]*)\s*=\s*[0-9a-f]{32,}',data) or re.search(rb'(?im)^\s*[0-9a-f]{32}\s*=\s*[0-9a-f]{32}',data):
                raise ValueError('Keys found in ZIP content: '+name)
            zipped=data.startswith(b'PK\x03\x04')
            tarred=data[257:262]==b'ustar' or data.startswith(b'\x1f\x8b')
            if not zipped and not tarred:return
            digest=hashlib.sha256(data).hexdigest()
            lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
            pinned={entry['sha256'] for entry in [lock['python'],*lock['wheels']]}
            if digest in pinned:return
            if depth==0 and digest not in approved:raise ValueError('Unapproved nested archive: '+name)
            # Descendants are already pinned by their approved source archive's
            # hash (e.g. Gradle's wrapper JAR). Still inspect every nested leaf.
            # Even hash-approved corresponding-source archives are opened and
            # scanned recursively; their names cannot hide game/key payloads.
            if zipped:
                with zipfile.ZipFile(io.BytesIO(data)) as inner:
                    for item in inner.infolist():
                        if item.is_dir():continue
                        if (item.external_attr>>16)&0o170000==0o120000:raise ValueError('Source archive symlink')
                        nested_files+=1;inspect(item.filename,inner.read(item),depth+1)
            else:
                with tarfile.open(fileobj=io.BytesIO(data),mode='r:*') as inner:
                    for item in inner:
                        if item.isdir() or item.type in (tarfile.XHDTYPE,tarfile.XGLTYPE):continue
                        if not item.isfile():raise ValueError('Source archive non-regular entry: '+item.name)
                        nested_files+=1;inspect(item.name,inner.extractfile(item).read(),depth+1)
        names=[i.filename for i in z.infolist()]
        if len(names)!=len(set(n.lower() for n in names)) or set(names)!=set(expected):
            raise ValueError('ZIP contains missing, duplicate or unapproved files')
        for name in names:
            safe_entry(name)
            data=z.read(name)
            if hashlib.sha256(data).hexdigest()!=expected[name]:raise ValueError('ZIP content hash mismatch: '+name)
            inspect(name,data)
    return dict(status='PASS',entries=names,files=len(names),nested_source_files=nested_files,sha256=sha256(path),game_files=0)


def write_zip(path,files):
    if path.exists():raise ValueError('Refusing to overwrite an existing archive')
    expected={name:sha256(src) for name,src in files.items()}
    for name in files:safe_entry(name)
    pending=path.with_suffix('.zip.part')
    try:
        with zipfile.ZipFile(pending,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=6) as z:
            for name,src in sorted(files.items()):z.write(src,name)
        report=verify_zip(pending,expected)
        pending.replace(path)
        report['sha256']=sha256(path)
        path.with_suffix('.contents.json').write_text(json.dumps(report,indent=2))
        return report
    finally:
        if pending.exists():pending.unlink()


def review_files():
    files={}
    for folder in ('installer','tools/release'):
        for path in (ROOT/folder).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts and path.suffix in ('.py','.cs','.cpp','.c','.h','.json','.ini','.txt','.def','.manifest'):
                name=path.relative_to(ROOT).as_posix();safe_entry(name);files[name]=path
    for name in ('StrikersSetup.exe','StrikersRecomp.exe'):
        files[name]=ROOT/'build/phase8/apps'/name
    files['LICENSE']=ROOT/'LICENSE'
    files['docs/PLAYING.md']=ROOT/'docs/PLAYING.md'
    files['docs/phase8-installer.md']=ROOT/'docs/phase8-installer.md'
    files['REVIEW-ONLY.txt']=ROOT/'tools/release/REVIEW-ONLY.txt'
    for name in ('job20_quick_battle_cold.seq','observe_soak_phase.py','player_handle_census.cpp',
                 'run_player_soak.py','test_player_crash.py','test_player_process.py',
                 'test_player_release.py','run_all_tests.py'):
        files['tools/audit/'+name]=ROOT/'tools/audit'/name
    return files


GATES=('embedded_setup','gui_self_test','failure_cases','quick_battle','unattended_cold_replay','group_match','soak_2h','crash_report','licenses_and_source')

def make_release(version,provenance,host=ROOT/'build/bin/strikers_native.exe',candidate=False,stage=None,acceptance=None):
    audit=audit_host(host,provenance)
    (ROOT/'build/phase8/release-host-audit.json').write_text(json.dumps(audit,indent=2))
    if not audit['game_free']:
        raise ValueError('Release BLOCKED: '+'; '.join(audit['reasons']))
    # Acceptance is required, not silently converted into a tools-only package.
    acceptance=Path(acceptance or ROOT/'build/phase8/player-acceptance.json')
    proof=json.loads(acceptance.read_text()) if acceptance.exists() else {}
    failures=[k for k in GATES if proof.get(k) is not True]
    if proof.get('host_sha256')!=audit['sha256']:failures.append('acceptance host hash')
    if failures and not candidate:raise ValueError('Release BLOCKED: '+', '.join(failures))
    # Prepared staging is an exact allowlist, never recursive-copy build/bin.
    stage=within(ROOT,stage or ROOT/'build/phase8/player-stage')
    manifest=json.loads((stage/'release-manifest.json').read_text())
    files={name:within(stage,stage/name) for name in manifest['files']}
    files['release-manifest.json']=stage/'release-manifest.json'
    if manifest.get('game_free_host') is not True:raise ValueError('Missing game-free staging assertion')
    if manifest.get('candidate_only') and not candidate:failures.append('staging is marked candidate-only')
    for name in (*RUNTIME_BINARIES,'StrikersSetup.exe','StrikersRecomp.exe','README-PLAYERS.txt','strikers.ini'):
        if name not in files:raise ValueError('Incomplete player staging: '+name)
    for name,path in files.items():
        if name!='release-manifest.json' and sha256(path)!=manifest['files'][name]:raise ValueError('Staging hash mismatch: '+name)
    if sha256(files['strikers_native.exe'])!=audit['sha256']:raise ValueError('Staging host differs from audited host')
    notices=json.loads(files['licenses/notices-manifest.json'].read_text())
    source=json.loads(files['corresponding-source/source-manifest.json'].read_text())
    if notices.get('status')!='PASS':failures.append('third-party notices')
    if source.get('complete') is not True or source.get('missing'):failures.append('corresponding source')
    for name,digest in source['files'].items():
        if 'corresponding-source/'+name not in files or sha256(files['corresponding-source/'+name])!=digest:
            raise ValueError('Corresponding-source content changed: '+name)
    for name,digest in source['binary_sha256'].items():
        if name not in files or sha256(files[name])!=digest:raise ValueError('Source manifest binary mismatch: '+name)
    # Job 23b acceptance belongs to one set, including launcher and networking.
    shipped_binaries=sorted(name for name in files if '/' not in name and Path(name).suffix.lower() in ('.exe','.dll'))
    for name in shipped_binaries:
        if proof.get('binary_sha256',{}).get(name)!=sha256(files[name]):
            failures.append('acceptance binary hash: '+name)
    if failures and not candidate:raise ValueError('Release BLOCKED: '+', '.join(failures))
    if candidate and 'RELEASE-CANDIDATE.txt' not in files:raise ValueError('Candidate staging marker missing')
    label=f'StrikersPC-RELEASE-CANDIDATE-{version}-INCOMPLETE' if candidate else f'StrikersPC-{version}'
    result=write_zip(ROOT/f'build/phase8/{label}.zip',files)
    result.update(candidate_only=candidate,release_gates='INCOMPLETE' if failures else 'PASS',open_gates=failures)
    (ROOT/f'build/phase8/{label}.gates.json').write_text(json.dumps(result,indent=2))
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--version',default='0.8.0-dev')
    p.add_argument('--host-provenance',type=Path)
    p.add_argument('--host',type=Path,default=ROOT/'build/bin/strikers_native.exe')
    p.add_argument('--check-host',action='store_true')
    p.add_argument('--review-tools',action='store_true')
    p.add_argument('--release-candidate',action='store_true',help='local review ZIP with explicit open gates; never grants player-release acceptance')
    p.add_argument('--stage',type=Path)
    p.add_argument('--acceptance',type=Path,help='acceptance evidence for this exact candidate; defaults to player-acceptance.json')
    a=p.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,63}',a.version):p.error('Invalid version')
    (ROOT/'build/phase8').mkdir(parents=True,exist_ok=True)
    try:
        if a.check_host:
            report=audit_host(a.host,a.host_provenance)
            (ROOT/'build/phase8/release-host-audit.json').write_text(json.dumps(report,indent=2))
            print(json.dumps(report,indent=2));raise SystemExit(0 if report['game_free'] else 1)
        elif a.review_tools:
            print(json.dumps(write_zip(ROOT/f'build/phase8/phase8-review-{a.version}.zip',review_files()),indent=2))
        else:print(json.dumps(make_release(a.version,a.host_provenance,a.host,a.release_candidate,a.stage,a.acceptance),indent=2))
    except (ValueError,FileNotFoundError) as e:
        print(str(e),file=sys.stderr);raise SystemExit(1)
