"""Collect exact vendored notices; fail if a required license is unavailable.

Uses runtime/provenance.json and actual shipped dependency revisions. Never
substitutes a generic license for a dependency's own copyright notices.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import sys
import zipfile
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,sha256,within


NOTICES={
    'GPL-3.0.txt':'LICENSE',
    'libopus-COPYING.txt':'deps/opus/COPYING',
    'FSR-LICENSE.txt':'runtime/gfx/fsr/LICENSE.txt',
    'SMAA-LICENSE.txt':'runtime/gfx/smaa/LICENSE.txt',
    'libzt-LICENSE.txt':'deps/libzt/LICENSE.txt',
    'ZeroTierOne-LICENSE.txt':'deps/libzt/ext/ZeroTierOne/LICENSE.txt',
    'ZeroTier-Apache-2.0.txt':'build/bin/ZeroTier-Apache-2.0.txt',
    'lwIP-COPYING.txt':'deps/libzt/ext/lwip/COPYING',
    'miniupnpc-LICENSE.txt':'deps/libzt/ext/ZeroTierOne/ext/miniupnpc/LICENSE',
    'libnatpmp-LICENSE.txt':'deps/libzt/ext/ZeroTierOne/ext/libnatpmp/LICENSE',
    'SDL3-LICENSE.txt':'deps/SDL3/LICENSE.txt',
    'ImGui-LICENSE.txt':'deps/imgui/LICENSE.txt',
    'glslang-LICENSE.txt':'tools/release/licenses/glslang-LICENSE.txt',
    'LLVM-LICENSE.txt':'tools/release/licenses/LLVM-9-LICENSE.txt',
    'Microsoft-CRT-Redistribution.txt':'tools/release/licenses/Microsoft-CRT-Redistribution.txt',
    'SPIRV-Tools-LICENSE.txt':'tools/release/licenses/SPIRV-Tools-LICENSE.txt',
    'SPIRV-Headers-LICENSE.txt':'tools/release/licenses/SPIRV-Headers-LICENSE.txt',
    'Microsoft-CRT-EULA.rtf':'tools/release/licenses/Microsoft-CRT-EULA.rtf',
    'Microsoft-Distributor-Terms.txt':'tools/release/licenses/Microsoft-Distributor-Terms.txt',
    'Vulkan-Headers-LICENSE.md':'deps/Vulkan-Headers/LICENSE.md',
    'Vulkan-Headers-Apache-2.0.txt':'deps/Vulkan-Headers/LICENSES/Apache-2.0.txt',
    'Vulkan-Headers-MIT.txt':'deps/Vulkan-Headers/LICENSES/MIT.txt',
}


def collect(out):
    out=within(ROOT,out);out.mkdir(parents=True,exist_ok=True)
    missing=[];sources={}
    for name,relative in NOTICES.items():
        source=ROOT/relative
        if not source.exists():missing.append(relative);continue
        shutil.copyfile(source,out/name)
        sources[name]=dict(source=relative,sha256=sha256(out/name))
    lock=json.loads((ROOT/'installer/toolchain.lock.json').read_text())
    for entry,member,name in zip([lock['python'],*lock['wheels']],
            ['LICENSE.txt','lz4-4.4.5.dist-info/licenses/LICENSE','pycryptodome-3.23.0.dist-info/LICENSE.rst'],
            ['Python-3.13.9-LICENSE.txt','lz4-4.4.5-LICENSE.txt','PyCryptodome-3.23.0-LICENSE.rst']):
        archive=ROOT/'installer/cache'/entry['url'].rsplit('/',1)[-1]
        if not archive.exists():missing.append('Pinned archive license: '+name);continue
        if sha256(archive)!=entry['sha256']:raise ValueError('Archive license hash mismatch: '+name)
        with zipfile.ZipFile(archive) as z:(out/name).write_bytes(z.read(member))
        sources[name]=dict(source=archive.relative_to(ROOT).as_posix()+'!'+member,sha256=sha256(out/name))
    port=ROOT/'deps/libzt/ext/lwip-contrib/ports/win32/sys_arch.c'
    text=port.read_text()
    header=text[:text.index('*/')+2]
    (out/'lwIP-Windows-port-LICENSE.txt').write_text(header+'\n')
    shutil.copyfile(ROOT/'runtime/provenance.json',out/'runtime-provenance.json')
    shutil.copyfile(ROOT/'tools/release/licenses/SOURCE-OFFER.txt',out/'SOURCE-OFFER.txt')
    if not (ROOT/'tools/release/licenses/Microsoft-Distributor-Terms.txt').exists():
        missing.append('Microsoft-Distributor-Terms.txt (including LLVM-bundled CRT distribution evidence)')
    report=dict(status='INCOMPLETE' if missing else 'PASS',missing=missing,notices=sources,
                corresponding_source='Required separately; not certified by this notice collector')
    (out/'notices-manifest.json').write_text(json.dumps(report,indent=2))
    (out/'THIRD-PARTY-NOTICES.txt').write_text(
        'Versions and source revisions: runtime-provenance.json\n'
        'Project runtime/tools: GPL-3.0-or-later (smo-recomp and ssbu-recomp provenance retained).\n'
        'libopus 1.5.2: BSD-3-Clause. FSR 1: MIT. SDL3: zlib. Dear ImGui: MIT.\n'
        'Pinned libzt / ZeroTierOne: retain original BSL notices and change-license\n'
        'Apache-2.0 text; see exact change dates in runtime-provenance.json.\n'
        'lwIP / Windows port / miniupnpc / libnatpmp: retain their exact notices.\n'
        'glslang 16.6.0 and embedded SPIRV-Tools require full upstream notices.\n'
        'LLVM/clang 9 and its bundled Microsoft runtime DLLs require their own\n'
        'redistribution notices. CPython and the two binary wheels retain their\n'
        'archive licenses. They are build tools, never game content.\n')
    return report


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,default=ROOT/'build/phase8/notices')
    a=p.parse_args();report=collect(a.out)
    print(json.dumps(report,indent=2));raise SystemExit(0 if report['status']=='PASS' else 1)
