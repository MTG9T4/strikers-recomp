"""Describe and compare the observed tagged save container without editing it."""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def inspect(path):
    b = path.read_bytes()
    magic, version, count = struct.unpack_from('<III', b)
    start = 12 + count * 12
    assert magic == 0xf28555ee and version == 2 and start <= len(b)
    entries = []
    end = 0
    for i in range(count):
        tag, size, offset = struct.unpack_from('<III', b, 12 + i * 12)
        assert offset == end and start + offset + size <= len(b)
        data = b[start + offset:start + offset + size]
        entries.append(dict(tag=f'{tag:08x}', size=size, offset=offset, sha256=hashlib.sha256(data).hexdigest(),
                            hex=data.hex() if size <= 32 else None))
        end = offset + size
    assert start + end == len(b), 'unaccounted trailing data'
    return dict(path=str(path), bytes=len(b), sha256=hashlib.sha256(b).hexdigest(), magic=f'{magic:08x}',
                version=version, records=count, payload_offset=start, payload_bytes=end, entries=entries)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('save', type=Path)
    p.add_argument('--before', type=Path)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    result = inspect(a.save)
    if a.before:
        before = inspect(a.before)
        old = {e['tag']: e for e in before['entries']}
        result['changed'] = [dict(before=old.get(e['tag']), after=e) for e in result['entries'] if old.get(e['tag']) != e]
        result['before'] = dict(path=str(a.before), sha256=before['sha256'])
    a.out.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'entries'}, indent=2))


if __name__ == '__main__':
    main()
