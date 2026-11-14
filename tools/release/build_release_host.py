"""Developer-only release host: generated SDK code/data stays in a local sidecar.

Two checked source adaptations are generated under build, never applied to
runtime files: rename the existing main binder; make the SDK install-list
length dynamic. All 65 host units otherwise use their original sources.
"""
import concurrent.futures
import argparse
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,run,sha256,within
sys.path.insert(0,str(ROOT/'tools/audit'))
from build_runtime import msvc_environment


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'build/phase8/release-host')
    p.add_argument('--main-import-lib',type=Path,help='optional existing import metadata; no game code is linked')
    p.add_argument('--local-group-test',action='store_true',help='non-shippable host permitting only numeric-loopback HTTP for local Worker acceptance')
    p.add_argument('--group-self-test',action='store_true',help='add the non-shippable native group acceptance driver')
    a=p.parse_args()
    out=within(ROOT,a.out);out.mkdir(parents=True,exist_ok=False)
    adapted=out/'adapted';adapted.mkdir();objdir=out/'obj';objdir.mkdir()
    # Rebuild the host from public source without any dump or generated game
    # header. Only factual target addresses/layout are needed by these units.
    layout_path=ROOT/'tools/release/host_layout.json'
    layout=json.loads(layout_path.read_text())['defines']
    import re
    for name,value in layout.items():
        if not re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*',name) or '\n' in value or '#' in value:
            raise ValueError('Invalid factual host-layout definition')
    (adapted/'image.h').write_text('/* Factual target layout; no game code or byte arrays. */\n'+
        '\n'.join('#define '+name+' '+value for name,value in layout.items())+'\n')
    fsr='/* Generated from runtime/gfx/fsr; do not edit. */\n'
    for symbol,filename in [('fsr_a','ffx_a.h'),('fsr_1','ffx_fsr1.h')]:
        data=(ROOT/'runtime/gfx/fsr'/filename).read_bytes()+b'\0'
        fsr+='static const char %s[] = {\n'%symbol
        fsr+='\n'.join(','.join(str(v) for v in data[i:i+128])+',' for i in range(0,len(data),128))+'\n};\n'
    (adapted/'fsr_sources.h').write_text(fsr)
    sys.path.insert(0,str(ROOT/'tools/lifter'))
    from build_exe import network_fingerprint
    fingerprint=network_fingerprint()
    (adapted/'ldn_fingerprint.h').write_text(f'#define STRIKERS_LDN_BUILD_DEFAULT 0x{fingerprint}ULL\n')
    transformations={
        'runtime/core/gamedll.c': [('void strikers_bind_game(void) {','void strikers_bind_game_core(void) {')],
        'runtime/services/nnservices.c': [('#include "sdklift_install.h"','#include "sdk_install_bridge.h"'),
                                        ('sizeof sdk_install_names / sizeof sdk_install_names[0]','player_sdk_install_count')],
    }
    overrides={}
    for original,replacements in transformations.items():
        text=(ROOT/original).read_text()
        for old,new in replacements:
            if text.count(old)!=1:raise ValueError('Source adaptation anchor changed: '+original)
            text=text.replace(old,new)
        target=adapted/Path(original).name;target.write_text(text)
        overrides[original]=target
    manifest=json.loads((ROOT/'runtime/runtime_sources.json').read_text())
    sources=manifest['host_sources']+['tools/release/sdk_host_bridge.c']
    if a.group_self_test:
        if not a.local_group_test:raise ValueError('Group self-test requires the explicitly non-shippable local test host')
        sources.append('tools/audit/job23b_group_driver.c')
    # generated_host_sources is intentionally absent. No byte arrays or lifted
    # SDK functions can enter this host through the developer source manifest.
    env=msvc_environment();cl=Path(env['VCToolsInstallDir'])/'bin/Hostx64/x64/cl.exe'
    includes=[adapted,ROOT/'tools/release']
    includes += [p for p in (ROOT/'runtime').iterdir() if p.is_dir()]
    includes += [ROOT/'deps'/p for p in ('SDL3/include','Vulkan-Headers/include','glslang/include','imgui')]
    flags=['/nologo','/c','/MD','/O2','/W3','/we4013','/D_CRT_SECURE_NO_WARNINGS','/DWIN32_LEAN_AND_MEAN']
    flags += ['/experimental:deterministic','/pathmap:'+str(ROOT)+'=Strikers']
    if a.local_group_test:flags.append('/DSTRIKERS_GROUP_LOCAL_TEST=1')
    flags += ['/I'+str(p) for p in includes]+['/D'+d for d in manifest['host_defines']]
    flags += ['/FI'+str(ROOT/'tools/release/sdk_remap.h')]
    def compile_one(item):
        i,original=item;source=overrides.get(original,ROOT/original);obj=objdir/f'{i:03}.obj'
        command=[cl,*flags,'/std:c++17' if source.suffix=='.cpp' else '/std:c11','/Fo'+str(obj),source]
        run(command,env,ROOT,objdir/f'{i:03}.log')
        return dict(source=original,sha256=sha256(ROOT/original),compiled_source=str(source),
                    compiled_sha256=sha256(source),object_sha256=sha256(obj),object=str(obj))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        records=list(pool.map(compile_one,enumerate(sources)))
    # Generate the host import library directly from the own game shim exports.
    # The developer .lib is only import metadata, never game code; its DLL name
    # is the fixed local strikers_game.dll.
    if a.main_import_lib:gamelib=a.main_import_lib.resolve()
    else:
        definitions=out/'game-imports.def'
        definitions.write_text('LIBRARY strikers_game.dll\nEXPORTS\n game_bind\n game_eh_bind\n')
        gamelib=out/'strikers_game.lib'
        run([cl.parent/'lib.exe','/nologo','/machine:x64','/def:'+str(definitions),'/out:'+str(gamelib)],env,out,out/'game-imports.log')
    if not gamelib.exists():raise ValueError('Missing own game-shim import metadata')
    libraries=[ROOT/'deps/SDL3/lib/x64/SDL3.lib']+[x for x in manifest['host_libraries'] if x!='SDL3.lib']
    exe=out/'strikers_native.exe'
    run([cl.parent/'link.exe','/nologo','/INCREMENTAL:NO','/SUBSYSTEM:CONSOLE','/OUT:'+str(exe),
         '/MANIFEST:EMBED','/MANIFESTINPUT:'+str(ROOT/'tools/release/host.manifest'),
         # Audit 09: movie_mf_present() needs these, or Windows N without the Media Feature Pack cannot start the host at all.
         '/DELAYLOAD:MFPlat.dll','/DELAYLOAD:MFReadWrite.dll','delayimp.lib',
         '/MAP:'+str(out/'strikers_native.map'),*[r['object'] for r in records],gamelib,*libraries],
        env,ROOT,out/'link.log')
    proof=dict(host_sha256=sha256(exe),sources=records,adaptations=transformations,
               main_import_lib=sha256(gamelib),main_import_lib_path=str(gamelib),
               network_fingerprint=fingerprint,fingerprint_header_sha256=sha256(adapted/'ldn_fingerprint.h'),
               manifest_sha256=sha256(ROOT/'tools/release/host.manifest'),
               artwork="none: the icon and start-up picture are read from the player's own dump (installer/game_art.py)",
               local_group_test=a.local_group_test,
               group_self_test=a.group_self_test,
               compiler=str(cl),compiler_sha256=sha256(cl),compiler_flags=flags,
               optimization='/O2',runtime_manifest_sha256=sha256(ROOT/'runtime/runtime_sources.json'),
               layout_sha256=sha256(layout_path),generated_host_sources=[],
               notes='Host layout generated from own factual JSON; import library generated from two own shim exports; no dump/lift is used')
    (out/'provenance.json').write_text(json.dumps(proof,indent=2))
    print('Built release-host candidate: '+str(exe))


if __name__=='__main__':main()
