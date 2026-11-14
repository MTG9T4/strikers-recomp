"""Turn a checked player stage into the files the in-game updater downloads, signed. A developer step.

  .venv/Scripts/python.exe tools/release/publish_update.py --stage build/phase8/<stage> --version 1.0.4 --notes "..."

Writes cloudflare/group/release-assets/release/: latest.json (every stage file with its SHA-256 and size),
latest.sig (Ed25519 over latest.json with cloudflare/group/.release-signing-key.pem), and files/<sha256> for each file;
a file over 20 MiB is split into files/<sha256>.<n>, because Workers static assets take 25 MiB at most. Files no
longer listed are removed, so the folder holds one release. Deploying the Worker publishes it; this script does not.
The stage must already have passed audit_stage.py: nothing here looks for game content again.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

ROOT = Path(__file__).resolve().parents[2]
PART = 20 << 20


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--notes', default='')
    p.add_argument('--audit', type=Path, help='the stage audit to require (default: <stage>/../<name>-stage-audit.json)')
    p.add_argument('--out', type=Path, default=ROOT / 'cloudflare/group/release-assets')
    p.add_argument('--key', type=Path, default=ROOT / 'cloudflare/group/.release-signing-key.pem')
    a = p.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+', a.version):
        raise SystemExit('Version is three numbers, such as 1.0.4')
    stage = a.stage.resolve()
    manifest = json.loads((stage / 'release-manifest.json').read_text())
    if manifest.get('version') != a.version or manifest.get('candidate_only'):
        raise SystemExit('The stage is not a player release of this version')
    out = a.out / 'release'
    files_dir = out / 'files'
    files_dir.mkdir(parents=True, exist_ok=True)
    files, keep = {}, set()
    for path in sorted(stage.rglob('*')):
        if not path.is_file():
            continue
        name = path.relative_to(stage).as_posix()
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if name != 'release-manifest.json' and manifest['files'].get(name) != digest:
            raise SystemExit('Stage file differs from its manifest: ' + name)
        entry = dict(sha256=digest, size=len(data))
        pieces = [(digest, data)] if len(data) <= PART else \
            [(f'{digest}.{i}', data[o:o + PART]) for i, o in enumerate(range(0, len(data), PART))]
        if len(pieces) > 1:
            entry['parts'] = [n for n, _ in pieces]
        for piece, chunk in pieces:
            keep.add(piece)
            target = files_dir / piece
            if not target.is_file() or target.stat().st_size != len(chunk):
                target.write_bytes(chunk)
        files[name] = entry
    for old in files_dir.iterdir():
        if old.name not in keep:
            old.unlink()
    release = dict(schema=1, version=a.version, notes=a.notes[:300], files=files)
    body = json.dumps(release, separators=(',', ':'), sort_keys=True).encode()
    from Crypto.PublicKey import ECC
    from Crypto.Signature import eddsa
    key = ECC.import_key(a.key.read_text())
    signature = eddsa.new(key, 'rfc8032').sign(body)
    (out / 'latest.json').write_bytes(body)
    (out / 'latest.sig').write_text(signature.hex())
    total = sum(f['size'] for f in files.values())
    print(json.dumps(dict(version=a.version, files=len(files), assets=len(keep), mb=total >> 20,
                          public_key=key.public_key().export_key(format='raw').hex())))


if __name__ == '__main__':
    main()
