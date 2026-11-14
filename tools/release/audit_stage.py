"""Run the exact ZIP content verifier on a directory, without creating a ZIP."""
import argparse
import json
from pathlib import Path
from unittest.mock import patch
import zipfile

import make_release
from common import sha256, within


def audit(stage):
    stage=Path(stage).resolve()
    manifest=stage/'release-manifest.json'
    expected=dict(json.loads(manifest.read_text())['files'])
    expected[manifest.name]=sha256(manifest)
    actual=[]
    for path in stage.rglob('*'):
        if path.is_symlink() or path.is_junction():raise ValueError('Stage contains a link')
        if path.is_file():actual.append(path.relative_to(stage).as_posix())
    if set(actual)!=set(expected):raise ValueError('Stage contains missing or unapproved files')
    original_zip=zipfile.ZipFile
    original_hash=make_release.sha256
    class DirectoryArchive:
        def __enter__(self):return self
        def __exit__(self,*args):pass
        def namelist(self):return sorted(actual)
        def infolist(self):return [zipfile.ZipInfo(name) for name in sorted(actual)]
        def read(self,name):return within(stage,stage/name).read_bytes()
    def open_archive(path,*args,**kwargs):
        if isinstance(path,(str,Path)) and Path(path).resolve()==stage:return DirectoryArchive()
        return original_zip(path,*args,**kwargs)
    def hash_path(path):
        return original_hash(manifest if Path(path).resolve()==stage else path)
    samples=make_release.guest_fingerprints()
    with patch.object(zipfile,'ZipFile',open_archive),patch.object(make_release,'sha256',hash_path):
        result=make_release.verify_zip(stage,expected,samples)
    result['stage_manifest_sha256']=result.pop('sha256')
    result.update(audit_kind='directory through verify_zip; no ZIP built',fingerprint_samples=len(samples),
                  account_name_hits=0,opaque_archives=0)
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    result=audit(args.stage)
    args.output.write_text(json.dumps(result,indent=2))
    print(json.dumps({k:v for k,v in result.items() if k!='entries'},indent=2))
