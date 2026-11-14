"""Build a deterministic source-only probe bundle; never deploys anything."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parents[2]
EXTENSIONS = {'.py', '.md', '.json', '.service', '.txt'}


def bundle(path):
    path = Path(path).resolve()
    if not path.is_relative_to(ROOT / 'build') or path.is_relative_to(ROOT / 'build/dump'):
        raise ValueError('bundle output must be inside build, outside build/dump')
    if path.exists():
        raise ValueError('bundle already exists; choose a new filename')
    files = sorted(p for p in (ROOT / 'server').rglob('*') if p.is_file() and p.suffix in EXTENSIONS and '__pycache__' not in p.parts)
    files += [ROOT / 'LICENSE', ROOT / 'tools/audit/test_nex.py']
    manifest = {'status': 'INCOMPLETE: localhost protocol probe, not a deployable game server',
                'contains_credentials': False, 'files': {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('xb') as raw, zipfile.ZipFile(raw, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        def add(name, data):
            info = zipfile.ZipInfo(name, (2026, 11, 5, 0, 0, 0));info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16;z.writestr(info, data)
        for p in files:
            add(p.relative_to(ROOT).as_posix(), p.read_bytes())
        add('BUNDLE.json', (json.dumps(manifest, indent=2, sort_keys=True)+'\n').encode())
    return {'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'files': len(files)}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', required=True, type=Path)
    args = ap.parse_args()
    print(json.dumps(bundle(args.out), indent=2))


if __name__ == '__main__':
    main()
