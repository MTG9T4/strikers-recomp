"""Convert the dump's NSO modules to ELF and report what the build is: ids, SDK, middleware, symbols, unwind data.

    .venv\\Scripts\\python -I scripts/identify.py

Reads build/dump/update/exefs (installer/game_files.py fills it), writes build/dump/update/elf/<module>.elf and
build/dump/update/identity.json.
"""
import argparse
import collections
import json
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'extract'))
import target as T              # noqa: E402
import nso                      # noqa: E402

MODULES = ('rtld', 'main', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'subsdk4', 'sdk')
# middleware / library markers worth knowing about early (network stack, engine, audio, video)
MARKERS = [rb'NintendoSDK_[A-Za-z0-9]+-\d+_\d+_\d+-Release', rb'NEX[_ -]?[0-9][0-9._]*', rb'nn::nex', rb'pia[_ ]?[0-9][0-9._]+',
           rb'nn::pia', rb'Pia[A-Za-z]*Version', rb'nn::ldn', rb'nn::friends', rb'nn::account', rb'nn::npln', rb'npln',
           rb'CriWare|CRI Middleware|CRIWARE', rb'Havok', rb'Wwise', rb'FMOD', rb'Bink', rb'Unreal', rb'Unity',
           rb'sead::', rb'agl::', rb'nw::', rb'nn::atk', rb'Next ?Level', rb'Mobiclip', rb'webm|libvpx', rb'Lua', rb'zstd|ZSTD']


def elf_sections(e):
    """name -> (addr, offset, size) of an ELF64 written by nso.nso_to_elf."""
    shoff, = struct.unpack_from('<Q', e, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', e, 0x3a)
    hdrs = [struct.unpack_from('<IIQQQQIIQQ', e, shoff + i * shentsize) for i in range(shnum)]
    stroff = hdrs[shstrndx][4]
    out = {}
    for h in hdrs:
        name = e[stroff + h[0]:e.index(b'\0', stroff + h[0])].decode()
        out[name] = (h[3], h[4], h[5])
    return out


def dynsym_stats(e, secs):
    if '.dynsym' not in secs or '.dynstr' not in secs:
        return {}
    _, off, size = secs['.dynsym']
    _, stroff, _ = secs['.dynstr']
    c = collections.Counter()
    sized_funcs = 0
    for i in range(size // 24):
        name_off, info, other, shndx, value, sz = struct.unpack_from('<IBBHQQ', e, off + i * 24)
        kind = info & 0xf
        defined = shndx != 0
        c[('defined' if defined else 'imported') + '_' + {0: 'notype', 1: 'object', 2: 'func'}.get(kind, str(kind))] += 1
        if defined and kind == 2 and sz:
            sized_funcs += 1
    d = dict(c)
    d['defined_sized_funcs'] = sized_funcs
    return d


def markers(data):
    found = {}
    for pat in MARKERS:
        hits = collections.Counter(m.group(0).decode('latin1') for m in re.finditer(pat, data))
        if hits:
            found[pat.decode()] = dict(hits.most_common(8))
    return found




def main():
    ap = argparse.ArgumentParser()
    a = ap.parse_args()
    exefs = os.path.join(T.DUMP, 'exefs')
    elfdir = os.path.join(T.DUMP, 'elf')
    os.makedirs(elfdir, exist_ok=True)
    ident = {'program_id_expected': '%016x' % T.PROGRAM_ID, 'modules': {}}

    npdm = open(os.path.join(exefs, 'main.npdm'), 'rb').read()
    ident['npdm_program_id'] = '%016x' % nso.npdm_program_id(npdm)
    nacp = os.path.join(T.DUMP, 'control.nacp')
    if os.path.isfile(nacp):
        ident['nacp'] = nso.nacp_info(open(nacp, 'rb').read())

    for m in MODULES:
        p = os.path.join(exefs, m)
        if not os.path.isfile(p):
            continue
        d = open(p, 'rb').read()
        info = nso.nso_info(d)
        e, _meta = nso.nso_to_elf(d)
        open(os.path.join(elfdir, m + '.elf'), 'wb').write(e)
        secs = elf_sections(e)
        image = b''.join(s['data'] for s in info['segments'])
        ident['modules'][m] = {
            'build_id': info['build_id'].rstrip('0') or '0',
            'segments_sha_ok': all(s['sha_ok'] for s in info['segments']),
            'text_size': info['segments'][0]['size'], 'ro_size': info['segments'][1]['size'],
            'data_size': info['segments'][2]['size'], 'bss': info['bss'],
            'sdk_version': nso.sdk_version(image),
            'sections': sorted(secs),
            'eh_frame_hdr': '.eh_frame_hdr' in secs, 'eh_frame': '.eh_frame' in secs,
            'dynsym': dynsym_stats(e, secs),
            'markers': markers(image),
        }
        print('%-8s %8d text  sha %s  sdk %s  eh_frame_hdr %s  syms %s' % (
            m, info['segments'][0]['size'], ident['modules'][m]['segments_sha_ok'], ident['modules'][m]['sdk_version'],
            ident['modules'][m]['eh_frame_hdr'], ident['modules'][m]['dynsym']))
    out = os.path.join(T.DUMP, 'identity.json')
    json.dump(ident, open(out, 'w'), indent=1)
    print('npdm program id', ident['npdm_program_id'], '| nacp', ident.get('nacp'))
    print('wrote', out)


if __name__ == '__main__':
    main()
