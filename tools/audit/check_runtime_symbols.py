"""Read MSVC COFF dependencies without linking; flag unaccounted host symbols.

GPL-3.0-or-later. Strikers project code. This cannot certify a final link, Windows
library versions, a game DLL export, or any generated SDK table's contents.
"""
from collections import defaultdict
import json
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
SDK = {'sdk_ctor_count', 'sdk_ctors', 'sdk_data', 'sdk_data_count', 'sdk_fn_count',
       'sdk_fns', 'sdk_host_name_count', 'sdk_host_names'}
CRT = set('abs cos exp fabs inet_ntop log log10 longjmp memchr memcmp memcpy memmove memset pow sin sqrt strcat strchr strcmp strcpy strlen strrchr strstr'.split())
WIN = {'SystemFunction036', 'CreateToolhelp32Snapshot', 'Thread32First', 'Thread32Next', 'K32GetProcessMemoryInfo', 'GUID_NULL'}


def symbols(path):
    data = path.read_bytes()
    machine, _, _, offset, count, _, _ = struct.unpack_from('<HHIIIHH', data)
    if machine != 0x8664:
        raise ValueError(f'{path}: expected x64 COFF')
    strings = data[offset + count * 18:]
    i = 0
    while i < count:
        entry = offset + i * 18
        value, section, _, storage, aux = struct.unpack_from('<IhHBB', data, entry + 8)
        raw = data[entry:entry + 8]
        if storage == 2:
            name = (strings[struct.unpack_from('<I', raw, 4)[0]:].split(b'\0')[0]
                    if raw[:4] == b'\0' * 4 else raw.split(b'\0')[0]).decode()
            yield name, section > 0 or section == 0 and value != 0
        i += 1 + aux


def main():
    out = ROOT / 'build/runtime-check'
    report = json.loads((out / 'results.json').read_text())
    definitions, uses = set(), defaultdict(list)
    for row in report['results']:
        if any(x in row['file'] for x in ('game_shim', 'vk_glslang_shim', 'vk_shaderc_shim')):
            continue
        if row['exit_code']:
            raise RuntimeError('compile must pass before checking dependencies')
        for name, defined in symbols(Path(row['object'])):
            if defined:
                definitions.add(name)
            else:
                uses[name].append(row['file'])
    groups = defaultdict(list)
    for name in sorted(uses.keys() - definitions):
        group = ('generated SDK tables' if name in SDK else
                 'game DLL import library' if name == '__imp_game_bind' else
                 'Windows SDL CRT libraries' if name in CRT | WIN or name.startswith(
                     ('__', '_', '?', 'SDL_', 'MF', 'IID_', 'GUID_')) else 'unaccounted')
        groups[group].append(name)
    (out / 'symbol-dependencies.json').write_text(json.dumps(groups, indent=2) + '\n')
    print(json.dumps({k: len(v) for k, v in groups.items()}, indent=2))
    if groups['unaccounted']:
        print('\n'.join(groups['unaccounted']))
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
