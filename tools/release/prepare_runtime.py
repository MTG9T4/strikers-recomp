"""Bundle app-local VC runtime files from the developer's official redist tree.

No install is run. The player never needs Visual Studio or a system redist
installer. Redistribution notices still have to pass collect_notices/source
review before make_release will allow a player package.
"""
import json
from pathlib import Path
import shutil
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from common import ROOT,sha256
sys.path.insert(0,str(ROOT/'tools/audit'))
from build_runtime import msvc_environment


def main():
    env=msvc_environment()
    source=Path(env['VCToolsRedistDir'])/'x64/Microsoft.VC143.CRT'
    # Freeze the exact locally available Microsoft distribution. A future
    # compiler/redist update requires a deliberate pin update, not latest-file discovery.
    if source.parent.parent.name!='14.44.35112':raise ValueError('Expected pinned MSVC redist 14.44.35112')
    names=('concrt140.dll','msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll',
           'msvcp140_atomic_wait.dll','msvcp140_codecvt_ids.dll','vcruntime140.dll',
           'vcruntime140_1.dll','vcruntime140_threads.dll')
    out=ROOT/'build/phase8/app-runtime';out.mkdir(parents=True,exist_ok=True)
    records={}
    for name in names:
        shutil.copyfile(source/name,out/name)
        records[name]=dict(source=str(source/name),sha256=sha256(out/name))
    (out/'runtime-provenance.json').write_text(json.dumps(dict(version='14.44.35112',files=records),indent=2))
    print(f'Prepared {len(records)} app-local runtime DLLs; no system installation')


if __name__=='__main__':main()
